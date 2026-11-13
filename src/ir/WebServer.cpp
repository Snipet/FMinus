// src/ir/WebServer.cpp: the web server's builtins' own part of the code generator (the table is Calls.cpp's, the
// library src/web/Web.h; docs/research/web/server.md, §4). In a build with FMINUS_WEB_SERVER:
//
// - A route, http_is's second argument, is a string literal checked where it is written (scanRoute), so that what can
//   never match a request is an error and not a route that is silently never taken: a method of GET, POST, PUT, PATCH
//   or DELETE (a GET route answers HEAD too), one space, and a path as the library serves one (Web.h, rule 2): "/" and
//   segments of letters, digits, '-', '.', '_' and '~', none empty, "." or "..", and %d, a decimal number of the
//   request's path, the only conversion, at most 8 of them, never two together nor one before a digit (each takes all
//   the digits there are).
// - Each call's route is recorded in the module's named metadata "fminus.web.routes" (ir::WebRoute, CodeGen.h), which
//   the host reads before it runs anything (Program::webRoutes), and each route literal, once, in the program's route
//   table: two private constants, the literals' addresses and their lengths, which every http_listen hands the library
//   (finishWebRoutes), so that the library answers 404 and 405 itself for what no route names. The table is made when
//   every function is lowered, since an http_is may come after the http_listen that needs it.
// - A query's name, http_query_float's and http_query_int's, is a literal of letters, digits, '_', '-' and '.', 1 to
//   64 bytes.
// - A reply's format, http_reply_text's, is C's printf's, in part: %d an int (or a bool, which C promotes to one), %g
//   and %.Ng with N from 1 to 17 a float or a double (which C promotes to one), %% a percent sign; checked as clang
//   checks printf's, in its words ("format specifies type 'double' but the argument has type 'int'"). The library gets
//   the format as written and the values as doubles (an int's exactly, a float's widened), and writes the text itself.
#include "ir/Lowering.h"

#include <llvm/IR/Constants.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Metadata.h>

#include <algorithm>
#include <array>

namespace fminus::ir::detail
{
    namespace
    {
        constexpr const char* kWebRoutesMetadata = "fminus.web.routes";

        // The methods a route may name (Web.h's server serves HEAD and OPTIONS too: a GET route answers HEAD, and
        // OPTIONS is no request a program answers).
        constexpr std::array<std::string_view, 5> kRouteMethods{"GET", "POST", "PUT", "PATCH", "DELETE"};

        constexpr int kMaxRouteNumbers = 8;   // Web.h's matcher keeps at most 8
        constexpr std::size_t kMaxQueryName = 64;

        // A byte as clang shows one in a message: itself when it is printable ASCII, else a hex escape.
        std::string shown(char c)
        {
            const auto byte = static_cast<unsigned char>(c);
            if (byte >= 0x20 && byte < 0x7f)
                return std::string(1, c);
            constexpr std::string_view kHex = "0123456789abcdef";
            return std::string("\\x") + kHex[byte >> 4] + kHex[byte & 15];
        }

        std::string shownText(std::string_view text)
        {
            std::string out;
            for (char c : text)
                out += shown(c);
            return out;
        }

        bool isDigit(char c)
        {
            return c >= '0' && c <= '9';
        }

        // A byte a path's segment may hold besides a %d (Web.h, rule 2): RFC 3986's unreserved characters.
        bool isPathByte(char c)
        {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || isDigit(c) || c == '-' || c == '.' || c == '_'
                   || c == '~';
        }
    } // namespace

    std::vector<FormatScan::Error> scanRoute(std::string_view text)
    {
        std::vector<FormatScan::Error> errors;
        const std::size_t space = text.find(' ');
        if (space == std::string_view::npos)
        {
            errors.push_back({0, "a route is a method, a space and a path, as \"PUT /gain\""});
            return errors;
        }
        const std::string_view method = text.substr(0, space);
        if (std::ranges::find(kRouteMethods, method) == kRouteMethods.end())
        {
            const bool served = method == "HEAD" || method == "OPTIONS";
            errors.push_back({0, "'" + shownText(method) + "' is not a method a route can name: GET, POST, PUT, PATCH "
                                     "or DELETE" + (method == "HEAD" ? " (a GET route answers HEAD too)"
                                                    : served     ? " (the library answers OPTIONS itself)"
                                                                 : "")});
            return errors;
        }
        const std::size_t start = space + 1;
        if (start < text.size() && text[start] == ' ')
        {
            errors.push_back({start, "a route has one space between its method and its path"});
            return errors;
        }
        if (start == text.size() || text[start] != '/')
        {
            errors.push_back({std::min(start, text.size() - 1), "a route's path begins with '/'"});
            return errors;
        }
        if (text.size() == start + 1)
            return errors;   // "/"
        int numbers = 0;
        std::size_t segment = start + 1;   // where the segment being read begins
        std::size_t numberEnd = 0;         // one past the last %d, or 0
        bool segmentHasNumber = false;
        const auto endSegment = [&](std::size_t end) {
            const std::string_view written = text.substr(segment, end - segment);
            if (written.empty())
            {
                errors.push_back({segment - 1, "a route's path has no empty segment"});
                return false;
            }
            if (!segmentHasNumber && (written == "." || written == ".."))
            {
                errors.push_back({segment, "a route's path has no '.' or '..' segment"});
                return false;
            }
            return true;
        };
        for (std::size_t i = start + 1; i < text.size(); ++i)
        {
            const char c = text[i];
            if (c == '/')
            {
                if (!endSegment(i))
                    return errors;
                segment = i + 1;
                segmentHasNumber = false;
                continue;
            }
            if (c == '%')
            {
                if (i + 1 < text.size() && text[i + 1] == 'd')
                {
                    if (numberEnd == i)
                    {
                        errors.push_back({i, "a %d right after another can never match: each takes all the digits "
                                             "there are"});
                        return errors;
                    }
                    if (++numbers > kMaxRouteNumbers)
                    {
                        errors.push_back({i, "a route takes at most 8 %d"});
                        return errors;
                    }
                    if (i + 2 < text.size() && isDigit(text[i + 2]))
                    {
                        errors.push_back({i + 2, "a digit right after %d can never match: %d takes all the digits "
                                                 "there are"});
                        return errors;
                    }
                    segmentHasNumber = true;
                    numberEnd = i + 2;
                    ++i;
                    continue;
                }
                if (i + 1 < text.size() && text[i + 1] == '%')
                {
                    errors.push_back({i, "a route's path cannot hold '%': %d, a number, is its only conversion"});
                    return errors;
                }
                // Anything else after a '%' is what clang would say of it in a format (Strings.cpp).
                const FormatScan scan = scanFormat(text.substr(i));
                if (!scan.errors.empty())
                    errors.push_back({i + scan.errors.front().offset, scan.errors.front().message});
                else
                    errors.push_back({i, "a route's path cannot hold '%': %d, a number, is its only conversion"});
                return errors;
            }
            if (c == '?' || c == '#')
            {
                errors.push_back({i, "a route is a path alone: a request's query is read with http_query_float and "
                                     "http_query_int"});
                return errors;
            }
            if (!isPathByte(c))
            {
                errors.push_back({i, "character '" + shown(c) + "' is not allowed in a route's path: letters, digits, "
                                     "'/', '-', '.', '_', '~' and %d"});
                return errors;
            }
        }
        endSegment(text.size());
        return errors;
    }

    ReplyFormatScan scanReplyFormat(std::string_view text)
    {
        constexpr std::string_view kDigits = "0123456789";
        constexpr std::string_view kConversions = "diouxXfFeEgGaAcsSpnmbBC%";   // clang's, for printf on Linux
        ReplyFormatScan scan;
        for (std::size_t i = 0; i < text.size(); ++i)
        {
            if (text[i] != '%')
                continue;
            std::size_t j = i + 1;
            const auto skip = [&](std::string_view set) {
                while (j < text.size() && set.find(text[j]) != std::string_view::npos)
                    ++j;
            };
            const auto next = [&](std::string_view part) { return text.substr(j).starts_with(part); };
            skip("-+ #0'");   // the flags
            if (next("*"))   // the width
                ++j;
            else
                skip(kDigits);
            const bool plain = j == i + 1;                  // no flag, no width
            std::size_t precision = std::string_view::npos;    // where its digits begin
            if (next("."))
            {
                ++j;
                precision = j;
                if (next("*"))
                    ++j;
                else
                    skip(kDigits);
            }
            const std::size_t length = j;
            if (next("hh") || next("ll"))
                j += 2;
            else if (next("I64") || next("I32"))
                j += 3;
            else if (j < text.size() && std::string_view("hlqjztLIw").find(text[j]) != std::string_view::npos)
                ++j;
            if (j == text.size())
                scan.errors.push_back({i, "incomplete format specifier"});
            else if (j == i + 1 && text[j] == 'd')
                scan.places.push_back({i, false});
            else if (j == i + 1 && text[j] == '%')
                ;   // a percent sign
            else if (j == i + 1 && text[j] == 'g')
                scan.places.push_back({i, true});
            else if (plain && precision == i + 2 && length == j && text[j] == 'g')
            {
                // %.Ng: N of one or two digits, 1 to 17, as the library reads it (Web.h)
                const std::string_view digits = text.substr(precision, j - precision);   // digits, "*" or none
                int value = 0;
                if (digits.size() <= 2 && digits.find('*') == std::string_view::npos)
                {
                    for (char d : digits)
                        value = value * 10 + (d - '0');
                }
                if (value >= 1 && value <= 17)
                    scan.places.push_back({i, true});
                else
                    scan.errors.push_back({i + 1, "a reply's %.Ng takes a precision N of 1 to 17"});
            }
            else if (kConversions.find(text[j]) == std::string_view::npos)
                scan.errors.push_back({j, "invalid conversion specifier '" + shown(text[j]) + "'"});
            else
                scan.errors.push_back({i + 1, "invalid conversion specifier '" + shownText(text.substr(i + 1, j - i)) +
                                                  "'"});
            i = j;
        }
        return scan;
    }

    // A route literal (scanRoute): its first error at its byte, and then false.
    bool Lowering::checkRoute(const ast::StringExpr& literal, SourceLocation where)
    {
        const std::vector<FormatScan::Error> errors = scanRoute(literal.value);
        if (errors.empty())
            return true;
        const FormatScan::Error& first = errors.front();
        const SourceLocation at = first.offset < literal.at.size() ? literal.at[first.offset] : where;
        if (first.message.starts_with("invalid conversion specifier") || first.message == "incomplete format specifier")
            diagnostics_.error(at, first.message, {{at, "a route takes %d, a decimal number of the path", {}}});
        else
            error(at, first.message);
        return false;
    }

    // A query's name: letters, digits, '_', '-' and '.', 1 to 64 bytes.
    bool Lowering::checkQueryName(const ast::StringExpr& literal, SourceLocation where)
    {
        const std::string_view text = literal.value;
        for (std::size_t i = 0; i < text.size(); ++i)
        {
            const char c = text[i];
            if ((c < 'a' || c > 'z') && (c < 'A' || c > 'Z') && !isDigit(c) && c != '_' && c != '-' && c != '.')
            {
                error(i < literal.at.size() ? literal.at[i] : where,
                      "character '" + shown(c) + "' is not allowed in a query's name: letters, digits, '_', '-' and "
                                                 "'.'");
                return false;
            }
        }
        if (text.empty() || text.size() > kMaxQueryName)
        {
            error(where, "a query's name is 1 to 64 bytes, not " + std::to_string(text.size()));
            return false;
        }
        return true;
    }

    // A reply's values, after its format in a call: checked against its places (scanReplyFormat), each converted to a
    // double, as C passes printf's (an int, a bool promoted to one, converted exactly; a float widened). Each error
    // where clang reports printf's, and then there are none. A place's value is lowered as an argument of its type is
    // (lowerAs): a %g's 0.1 is C's double 0.1, as printf's is.
    std::optional<std::vector<llvm::Value*>> Lowering::replyArguments(const ast::StringExpr& literal,
                                                                     const std::vector<const ast::Expr*>& values)
    {
        const ReplyFormatScan scan = scanReplyFormat(literal.value);
        bool fine = scan.errors.empty();
        const auto at = [&](std::size_t offset) {
            return offset < literal.at.size() ? literal.at[offset] : literal.at.empty() ? SourceLocation{} : literal.at[0];
        };
        for (const FormatScan::Error& wrong : scan.errors)
            diagnostics_.error(at(wrong.offset), wrong.message,
                               {{at(wrong.offset),
                                 "a reply's format takes %d, an int; %g and %.Ng (N from 1 to 17), a float or a "
                                 "double; and %%, a percent sign",
                                 {}}});
        const ast::Type real = ast::Type::Double;
        std::vector<llvm::Value*> converted;
        for (std::size_t k = 0; k < values.size(); ++k)
        {
            const ast::Expr& arg = *values[k];
            const bool placed = k < scan.places.size();
            const bool wantsReal = placed && scan.places[k].real;
            const std::string wanted = wantsReal ? "'double'" : "'int'";
            if (isString(arg))   // C's char array, passed as a char*
            {
                if (placed)
                    error(begin(arg), "format specifies type " + wanted + " but the argument has type 'char *'");
                fine = false;
                converted.push_back(poison(real).ir);
                continue;
            }
            const Value value = wantsReal ? lowerAs(arg, real) : lower(arg);
            fine = fine && !value.failed;
            if (!placed || value.failed)
            {
                converted.push_back(poison(real).ir);
                continue;
            }
            const bool integer = value.type.kind == ast::Kind::Int || value.type == ast::Type::Bool;
            const bool floating = value.type == ast::Type::Float || value.type == ast::Type::Double;
            if (wantsReal ? !floating : !integer)
            {
                error(begin(arg), "format specifies type " + wanted + " but the argument has type "
                                      + quoted(ast::typeName(value.type)));
                fine = false;
                converted.push_back(poison(real).ir);
                continue;
            }
            if (wantsReal)
                converted.push_back(convert(value, real, arg.location, Use::Passing));
            else
                converted.push_back(builder_.CreateSIToFP(convert(value, ast::Type::Int, arg.location, Use::Passing),
                                                          builder_.getDoubleTy(), "format.int"));
        }
        if (values.size() < scan.places.size() && scan.errors.empty())
        {
            error(at(scan.places[values.size()].offset + 1), "more '%' conversions than data arguments");
            fine = false;
        }
        else if (values.size() > scan.places.size() && scan.errors.empty())
        {
            error(begin(*values[scan.places.size()]), "data argument not used by format string");
            fine = false;
        }
        if (!fine)
            return std::nullopt;
        return converted;
    }

    // A reply's values, in an array of doubles of the function's stack frame: its address, or null for none.
    llvm::Value* Lowering::replyValues(const std::vector<llvm::Value*>& values)
    {
        if (values.empty())
            return llvm::ConstantPointerNull::get(builder_.getPtrTy());
        llvm::AllocaInst* array = allocate(llvm::ArrayType::get(builder_.getDoubleTy(), values.size()), "format.values");
        for (std::size_t k = 0; k < values.size(); ++k)
            tag(builder_.CreateStore(values[k], builder_.CreateConstInBoundsGEP2_32(array->getAllocatedType(), array, 0,
                                                                                   static_cast<unsigned>(k))),
                ast::Type::Double);
        return array;
    }

    // The call's route, its method and its path as a pattern, each %d a "*", where it is: the module's named
    // metadata, which ir::webRoutes reads. And the literal, once, for the route table.
    void Lowering::recordWebRoute(SourceLocation location, const ast::StringExpr& literal)
    {
        const std::string& text = literal.value;   // checked: a method, one space, a path
        const std::size_t space = text.find(' ');
        std::string pattern;
        for (std::size_t i = space + 1; i < text.size(); ++i)
        {
            if (text[i] == '%')   // %d
            {
                pattern += '*';
                ++i;
            }
            else
                pattern += text[i];
        }
        if (std::ranges::find(webRoutes_, text) == webRoutes_.end())
            webRoutes_.push_back(text);
        if (relowering_ > 0)
            return;   // a deferred statement lowered again: recorded the first time (Statements.cpp)
        llvm::LLVMContext& context = module_.getContext();
        llvm::Type* i32 = llvm::Type::getInt32Ty(context);
        llvm::NamedMDNode* routes = module_.getOrInsertNamedMetadata(kWebRoutesMetadata);
        routes->addOperand(llvm::MDTuple::get(
            context, {llvm::MDString::get(context, text.substr(0, space)), llvm::MDString::get(context, pattern),
                      llvm::MDString::get(context, diagnostics_.file(location.file).name),
                      llvm::ConstantAsMetadata::get(llvm::ConstantInt::getSigned(i32, location.line)),
                      llvm::ConstantAsMetadata::get(llvm::ConstantInt::getSigned(i32, location.column))}));
    }

    // What http_listen passes the library: the program's web context (Web.cpp), and its route table, which
    // finishWebRoutes puts in place of these nulls once every function is lowered.
    std::vector<llvm::Value*> Lowering::listenArguments(SourceLocation location)
    {
        llvm::Value* none = llvm::ConstantPointerNull::get(builder_.getPtrTy());
        return {webContext(location), none, none, builder_.getInt32(0)};
    }

    // The program's route table, two private constants: the route literals' addresses ("fminus.web.routes", each a
    // private constant of its bytes) and their lengths ("fminus.web.route.lengths"), each literal once; handed to the
    // library by every http_listen, with their count. A program with no route hands it none.
    void Lowering::finishWebRoutes()
    {
        if (webListens_.empty() || webRoutes_.empty())
            return;
        llvm::LLVMContext& context = module_.getContext();
        llvm::Type* pointer = llvm::PointerType::getUnqual(context);
        llvm::Type* i32 = llvm::Type::getInt32Ty(context);
        std::vector<llvm::Constant*> addresses;
        std::vector<llvm::Constant*> lengths;
        for (const std::string& route : webRoutes_)
        {
            llvm::Constant* bytes = llvm::ConstantDataArray::getString(context, route, false);
            auto* literal = new llvm::GlobalVariable(module_, bytes->getType(), true, llvm::GlobalValue::PrivateLinkage,
                                                     bytes, ".route");
            literal->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
            literal->setAlignment(llvm::Align(1));
            addresses.push_back(literal);
            lengths.push_back(llvm::ConstantInt::get(i32, route.size()));
        }
        const auto table = [&](llvm::Type* element, const std::vector<llvm::Constant*>& entries, const char* name) {
            auto* type = llvm::ArrayType::get(element, entries.size());
            auto* global = new llvm::GlobalVariable(module_, type, true, llvm::GlobalValue::PrivateLinkage,
                                                    llvm::ConstantArray::get(type, entries), name);
            global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
            return global;
        };
        llvm::GlobalVariable* routes = table(pointer, addresses, "fminus.web.routes");
        llvm::GlobalVariable* routeLengths = table(i32, lengths, "fminus.web.route.lengths");
        for (llvm::CallInst* listen : webListens_)
        {
            listen->setArgOperand(1, routes);
            listen->setArgOperand(2, routeLengths);
            listen->setArgOperand(3, llvm::ConstantInt::get(i32, webRoutes_.size()));
        }
    }
} // namespace fminus::ir::detail

namespace fminus::ir
{
    std::vector<WebRoute> webRoutes(const llvm::Module& module)
    {
        std::vector<WebRoute> routes;
        const llvm::NamedMDNode* named = module.getNamedMetadata(detail::kWebRoutesMetadata);
        if (named == nullptr)
            return routes;
        const auto text = [](const llvm::MDNode& node, unsigned i) {
            return llvm::cast<llvm::MDString>(node.getOperand(i))->getString().str();
        };
        const auto number = [](const llvm::MDNode& node, unsigned i) {
            return static_cast<int>(llvm::mdconst::extract<llvm::ConstantInt>(node.getOperand(i))->getSExtValue());
        };
        for (const llvm::MDNode* node : named->operands())
            routes.push_back({text(*node, 0), text(*node, 1), text(*node, 2), number(*node, 3), number(*node, 4)});
        return routes;
    }
} // namespace fminus::ir
