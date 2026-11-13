// src/ir/Web.cpp: the web library's builtins' own part of the code generator (the table is Calls.cpp's, the library
// src/web/Web.h). In a build with FMINUS_WEB:
//
// - A URL, http_get's and http_post's first argument, is a format (Strings.cpp), checked further where it is written,
//   so that what can never be a request is an error and not a refusal at run time: printable ASCII with no space and
//   no backslash (no CR or LF, so nothing a program writes can add a line to a request), a scheme of http or https,
//   and no credentials (an '@' in the authority: the library refuses userinfo of any kind). A content type, http_post's
//   fourth, is printable ASCII, at most 255 bytes, as the library takes one.
// - Each call's URL is recorded in the module's named metadata "fminus.web.requests" (ir::WebRequest, CodeGen.h): the
//   host reads them all before it runs anything (Program::webRequests), since every URL is a literal.
// - The program's web context is one global of its module, ir::kWebContextSymbol, a pointer the host sets (null: no
//   network), which every call that starts a request loads and passes to the library first, as the loop budget is one
//   global the host sets (opt/LoopBudget.h).
// - The status codes are names a program uses without declaring them, HTTP_PENDING, HTTP_TRANSPORT, ... (webStatus):
//   int constants, as an enumeration's are, in a scope outside the file's, so that a program's own declaration of
//   such a name hides it, as a block's hides the file's. They are Web.h's FMINUS_WEB_* values. In a build without the
//   web library each is still a known name, whose use is the "leaves out" error (Calls.cpp's notBuilt).
#include "ir/Lowering.h"

#include "fminus/Config.h"

#include <llvm/IR/Constants.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Metadata.h>

#include <array>

namespace fminus::ir::detail
{
    namespace
    {
        constexpr const char* kWebRequestsMetadata = "fminus.web.requests";

        // The status codes (src/web/Web.h's FMINUS_WEB_*: tests/unit/WebTest.cpp checks that they agree).
        struct WebStatus
        {
            std::string_view name;
            std::int32_t value;
        };
        constexpr std::array kWebStatuses{
            WebStatus{"HTTP_PENDING", 0},     WebStatus{"HTTP_TRANSPORT", -1},   WebStatus{"HTTP_TIMEOUT", -2},
            WebStatus{"HTTP_CANCELLED", -3},  WebStatus{"HTTP_REFUSED", -4},     WebStatus{"HTTP_LIMIT", -5},
            WebStatus{"HTTP_CERTIFICATE", -6}, WebStatus{"HTTP_INVALID", -7},    WebStatus{"HTTP_FAILED", -8},
        };

        std::string shownByte(char c)
        {
            constexpr std::string_view kHex = "0123456789abcdef";
            const auto byte = static_cast<unsigned char>(c);
            return std::string("\\x") + kHex[byte >> 4] + kHex[byte & 15];
        }

        bool startsWithNoCase(std::string_view text, std::string_view prefix)
        {
            if (text.size() < prefix.size())
                return false;
            for (std::size_t i = 0; i < prefix.size(); ++i)
            {
                char c = text[i];
                if (c >= 'A' && c <= 'Z')
                    c = static_cast<char>(c - 'A' + 'a');
                if (c != prefix[i])
                    return false;
            }
            return true;
        }
    } // namespace

    // A URL literal: every byte printable ASCII but a space and a backslash, a scheme of http or https, and no '@'
    // before the path. Each error at its byte, and then false.
    bool Lowering::checkUrl(const ast::StringExpr& literal, SourceLocation where)
    {
        const std::string_view text = literal.value;
        const auto at = [&](std::size_t offset) { return offset < literal.at.size() ? literal.at[offset] : where; };
        for (std::size_t i = 0; i < text.size(); ++i)
        {
            const auto byte = static_cast<unsigned char>(text[i]);
            if (byte <= 0x20 || byte >= 0x7f || text[i] == '\\')
            {
                error(at(i), "character '" + (byte == 0x20 || text[i] == '\\' ? std::string(1, text[i])
                                                                               : shownByte(text[i]))
                                 + "' is not allowed in a URL: it is printable ASCII, without spaces or backslashes");
                return false;
            }
        }
        std::size_t authority = 0;
        if (startsWithNoCase(text, "https://"))
            authority = 8;
        else if (startsWithNoCase(text, "http://"))
            authority = 7;
        else
        {
            error(at(0), "a URL's scheme is http or https, as 'https://example.com/a.json'");
            return false;
        }
        const std::size_t end = std::min(text.find_first_of("/?#", authority), text.size());
        if (const std::size_t sign = text.find('@', authority); sign < end)
        {
            error(at(sign), "a URL cannot carry credentials ('@' before its host): the web library refuses them");
            return false;
        }
        return true;
    }

    // A content type literal: printable ASCII, at most 255 bytes, as the library takes one.
    bool Lowering::checkContentType(const ast::StringExpr& literal, SourceLocation where)
    {
        const std::string_view text = literal.value;
        for (std::size_t i = 0; i < text.size(); ++i)
        {
            const auto byte = static_cast<unsigned char>(text[i]);
            if (byte < 0x20 || byte >= 0x7f)
            {
                error(i < literal.at.size() ? literal.at[i] : where,
                      "character '" + shownByte(text[i]) + "' is not allowed in a content type: it is printable ASCII");
                return false;
            }
        }
        if (text.size() > 255)
        {
            error(where, "a content type is at most 255 bytes, not " + std::to_string(text.size()));
            return false;
        }
        return true;
    }

    // The call's method and its URL as a pattern, each %d a "*" and %% a "%", where it is: the module's named
    // metadata, which ir::webRequests reads.
    void Lowering::recordWebRequest(SourceLocation location, std::string_view method, const ast::StringExpr& literal)
    {
        if (relowering_ > 0)
            return;   // a deferred statement lowered again: recorded the first time (Statements.cpp)
        std::string pattern;
        const std::string& text = literal.value;
        for (std::size_t i = 0; i < text.size(); ++i)
        {
            if (text[i] == '%' && i + 1 < text.size())   // the format is checked: %d or %%
            {
                pattern += text[i + 1] == 'd' ? '*' : '%';
                ++i;
            }
            else
                pattern += text[i];
        }
        llvm::LLVMContext& context = module_.getContext();
        llvm::Type* i32 = llvm::Type::getInt32Ty(context);
        llvm::NamedMDNode* requests = module_.getOrInsertNamedMetadata(kWebRequestsMetadata);
        requests->addOperand(llvm::MDTuple::get(
            context, {llvm::MDString::get(context, method), llvm::MDString::get(context, pattern),
                      llvm::MDString::get(context, diagnostics_.file(location.file).name),
                      llvm::ConstantAsMetadata::get(llvm::ConstantInt::getSigned(i32, location.line)),
                      llvm::ConstantAsMetadata::get(llvm::ConstantInt::getSigned(i32, location.column))}));
    }

    // The program's web context, loaded where a request starts: the module's global, defined here at its first use,
    // null until the host sets it. The host sets it once, before it calls anything (Program::compile, an executable's
    // C main), so a plain load reads it. A function or a constant of the program of its name (whose symbol would take
    // the name) is an error, once.
    llvm::Value* Lowering::webContext(SourceLocation location)
    {
        if ((functions_.contains(kWebContextSymbol) || fileScope_.contains(kWebContextSymbol)) && !webContextClash_)
        {
            webContextClash_ = true;
            error(location, std::string("the web library reserves '") + kWebContextSymbol
                                + "' for the program's context, and this program declares something of that name: "
                                  "rename it");
        }
        llvm::GlobalVariable* global = module_.getNamedGlobal(kWebContextSymbol);
        if (global == nullptr)
        {
            global = new llvm::GlobalVariable(module_, builder_.getPtrTy(), false, llvm::GlobalValue::ExternalLinkage,
                                              llvm::ConstantPointerNull::get(builder_.getPtrTy()), kWebContextSymbol);
            global->setAlignment(llvm::Align(8));
        }
        return builder_.CreateAlignedLoad(builder_.getPtrTy(), global, llvm::Align(8), "web.context");
    }

    // The status code `name` names, in a build with the web library, as an enumeration constant's binding; nullptr
    // for any other name, and in a build without it.
    const Lowering::Binding* Lowering::webStatus(const std::string& name) const
    {
        if (FMINUS_HAS_WEB == 0)
            return nullptr;
        static const std::array<Binding, kWebStatuses.size()> kBindings = [] {
            std::array<Binding, kWebStatuses.size()> bindings{};
            for (std::size_t i = 0; i < kWebStatuses.size(); ++i)
                bindings[i] = Binding{nullptr, true, ast::Type::Int, {}, kWebStatuses[i].value};
            return bindings;
        }();
        for (std::size_t i = 0; i < kWebStatuses.size(); ++i)
        {
            if (kWebStatuses[i].name == name)
                return &kBindings[i];
        }
        return nullptr;
    }

    // In a build without the web library, a use of a status code's name is the error that names the option: true when
    // it reported it.
    bool Lowering::unbuiltWebStatus(SourceLocation location, const std::string& name)
    {
        if (isBuilt(Feature::Web))
            return false;
        for (const WebStatus& status : kWebStatuses)
        {
            if (status.name == name)
            {
                error(location, notBuilt(name, Feature::Web));
                return true;
            }
        }
        return false;
    }
} // namespace fminus::ir::detail

namespace fminus::ir
{
    std::vector<WebRequest> webRequests(const llvm::Module& module)
    {
        std::vector<WebRequest> requests;
        const llvm::NamedMDNode* named = module.getNamedMetadata(detail::kWebRequestsMetadata);
        if (named == nullptr)
            return requests;
        const auto text = [](const llvm::MDNode& node, unsigned i) {
            return llvm::cast<llvm::MDString>(node.getOperand(i))->getString().str();
        };
        const auto number = [](const llvm::MDNode& node, unsigned i) {
            return static_cast<int>(
                llvm::mdconst::extract<llvm::ConstantInt>(node.getOperand(i))->getSExtValue());
        };
        for (const llvm::MDNode* node : named->operands())
            requests.push_back({text(*node, 0), text(*node, 1), text(*node, 2), number(*node, 3), number(*node, 4)});
        return requests;
    }
} // namespace fminus::ir
