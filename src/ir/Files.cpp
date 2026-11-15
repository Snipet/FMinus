// src/ir/Files.cpp: the file interface's builtins' own part of the code generator (the table is Calls.cpp's, the
// runtime runtime/Files.h; docs/research/files/README.md, §4). In a build with FMINUS_FILES:
//
// - A file's name, file_read's, file_write's and file_append's first argument, and a listing's pattern, file_list's,
//   is a format (Strings.cpp) checked further where it is written (scanFileName), against the rules the runtime keeps
//   (runtime/Files.h), so that a name no policy could ever let through is an error at its byte and not a refusal at
//   run time: a folder's name and a path inside it; segments of UTF-8 without a control character or any of
//   \ : * ? " < > |, none beginning with '.' or a space nor ending with either, none a device name of Windows; an
//   extension the literal writes; at most 16 segments. And the build's ceilings, which is what deciding them when
//   FMinus is built buys: a type outside FMINUS_FILES_TYPES, an absolute name in a build without FMINUS_FILES_ANYWHERE,
//   and an absolute name written, are errors too. The runtime checks the name again in full once its %d are filled
//   in: this check is the courtesy, that one the rule.
// - A name may name its values by holes, `{name}` (Strings.cpp), an int or a string, in any segment but the folder and
//   in no extension. A string joins the segment it is written in, and the runtime checks the name filled in by the
//   rules this file checks a literal's segments by (a '/' or a '*' from the string, a leading '.', a device's name,
//   the lengths): one that breaks a rule is FILE_INVALID, a status the program reads (runtime/Files.cpp).
// - Each call's name is recorded in the module's named metadata "fminus.files" (ir::FileUse, CodeGen.h): the host reads
//   them all before it runs anything (Program::files), since every name is a literal.
// - The program's file context is one global of its module, ir::kFilesContextSymbol, a pointer the host sets (null: no
//   files, the default), which every call that names a file loads and passes to the runtime first, as the web
//   context is (Web.cpp).
// - The status codes are names a program uses without declaring them, FILE_OK, FILE_MISSING, ... (fileStatus), as the
//   web library's are: in a scope outside the file's, so that a program's own declaration of one hides it. They are
//   Files.h's FMINUS_FILE_* values. In a build without the file interface each is still a known name, whose use is the
//   "leaves out" error (Calls.cpp's notBuilt).
#include "ir/Lowering.h"

#include "fminus/Config.h"

#include <llvm/IR/Constants.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Metadata.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/TargetParser/Triple.h>

#include <algorithm>
#include <array>
#include <string>
#include <utility>
#include <vector>

namespace fminus::ir::detail
{
    namespace
    {
        constexpr const char* kFilesMetadata = "fminus.files";

        // The status codes (runtime/Files.h's FMINUS_FILE_*: tests/unit/FileBuiltinsTest.cpp checks that they agree).
        struct FileStatus
        {
            std::string_view name;
            std::int32_t value;
        };
        constexpr std::array kFileStatuses{
            FileStatus{"FILE_OK", 0},      FileStatus{"FILE_MISSING", -1}, FileStatus{"FILE_REFUSED", -2},
            FileStatus{"FILE_DENIED", -3}, FileStatus{"FILE_LIMIT", -4},   FileStatus{"FILE_FAILED", -5},
            FileStatus{"FILE_INVALID", -6},
        };

        // The rules' sizes (runtime/Files.cpp's).
        constexpr std::size_t kMostNameBytes = 1024;
        constexpr std::size_t kMostSegments = 16;
        constexpr std::size_t kMostSegmentBytes = 255;
        constexpr std::size_t kMostExtension = 16;
        constexpr std::size_t kMostFolderName = 64;

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

        char lower(char c)
        {
            return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
        }

        bool letterOrDigit(char c)
        {
            return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        }

        bool sameLetters(std::string_view a, std::string_view b)
        {
            if (a.size() != b.size())
                return false;
            for (std::size_t i = 0; i < a.size(); ++i)
            {
                if (lower(a[i]) != lower(b[i]))
                    return false;
            }
            return true;
        }

        // The code point UTF-8 starts at `s[i]` and its length, or -1 when it is not well formed (overlong, a
        // surrogate, past U+10FFFF, cut short), as runtime/Files.cpp reads one.
        long codePoint(std::string_view s, std::size_t i, std::size_t& length)
        {
            const auto byte = [&](std::size_t k) { return static_cast<unsigned char>(s[k]); };
            const unsigned char b0 = byte(i);
            length = 1;
            if (b0 < 0x80)
                return b0;
            std::size_t extra = 0;
            long cp = 0;
            long least = 0;
            if (b0 >= 0xc2 && b0 <= 0xdf)
            {
                extra = 1;
                cp = b0 & 0x1f;
                least = 0x80;
            }
            else if (b0 >= 0xe0 && b0 <= 0xef)
            {
                extra = 2;
                cp = b0 & 0x0f;
                least = 0x800;
            }
            else if (b0 >= 0xf0 && b0 <= 0xf4)
            {
                extra = 3;
                cp = b0 & 0x07;
                least = 0x10000;
            }
            else
                return -1;
            if (i + extra >= s.size())
                return -1;
            for (std::size_t k = 1; k <= extra; ++k)
            {
                const unsigned char b = byte(i + k);
                if ((b & 0xc0) != 0x80)
                    return -1;
                cp = (cp << 6) | (b & 0x3f);
            }
            if (cp < least || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
                return -1;
            length = extra + 1;
            return cp;
        }

        // Whether `base` (a segment before its first '.', spaces at its end aside, each %d a digit) is a device name
        // of Windows: CON, PRN, AUX, NUL, CONIN$, CONOUT$, or COM or LPT and a digit (0 to 9, or a superscript 1, 2 or
        // 3), in any letter case.
        bool deviceName(std::string_view base)
        {
            while (!base.empty() && base.back() == ' ')
                base.remove_suffix(1);
            for (const std::string_view device : {"CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$"})
            {
                if (sameLetters(base, device))
                    return true;
            }
            if (base.size() < 4 || (!sameLetters(base.substr(0, 3), "COM") && !sameLetters(base.substr(0, 3), "LPT")))
                return false;
            const std::string_view number = base.substr(3);
            if (number.size() == 1)
                return number[0] >= '0' && number[0] <= '9';
            return number == "\xc2\xb9" || number == "\xc2\xb2" || number == "\xc2\xb3";
        }

        // A piece of a literal: the text a name's rules read, each %d a digit ('0'), each %s (a string's hole) a letter
        // ('a': a string is checked when it is filled in) and %% a '%', and for each of its bytes the offset in the
        // literal of the byte it stands for, and whether it is a place's.
        struct Spelled
        {
            std::string text;
            std::vector<std::size_t> offsets;
            std::vector<bool> places;
        };

        Spelled spelled(std::string_view literal)
        {
            Spelled out;
            for (std::size_t i = 0; i < literal.size(); ++i)
            {
                const bool number = literal[i] == '%' && i + 1 < literal.size() && literal[i + 1] == 'd';
                const bool text = literal[i] == '%' && i + 1 < literal.size() && literal[i + 1] == 's';
                const bool place = number || text;
                const bool percent = literal[i] == '%' && i + 1 < literal.size() && literal[i + 1] == '%';
                out.text += number ? '0' : text ? 'a' : literal[i];
                out.offsets.push_back(i);
                out.places.push_back(place);
                if (place || percent)
                    ++i;
            }
            return out;
        }
    } // namespace

    std::vector<FormatScan::Error> scanFileName(std::string_view literal, const FileNameRules& rules)
    {
        std::vector<FormatScan::Error> errors;
        const auto fail = [&](std::size_t offset, std::string message) {
            errors.push_back({offset, std::move(message)});
            return errors;
        };
        const Spelled name = spelled(literal);
        const std::string& text = name.text;
        const auto at = [&](std::size_t k) { return k < name.offsets.size() ? name.offsets[k] : literal.size(); };
        const char* example = rules.pattern ? "\"kit/*.f32\"" : "\"presets/7.json\"";
        if (text.empty())
            return fail(0, std::string("a file's name is a folder its host maps, '/' and a path inside it, as ")
                               + example);

        // Absolute: '/' on Linux and macOS; a drive letter, ':' and '/' on Windows. Either form is one, wherever it
        // is compiled: in a build without absolute names an error that says so, and in one with them, the other
        // system's form an error that says which this one takes.
        const bool slash = text.front() == '/';
        const bool drive = text.size() >= 3 && (lower(text[0]) >= 'a' && lower(text[0]) <= 'z') && text[1] == ':'
                           && text[2] == '/';
        std::size_t root = 0;
        if (slash || drive)
        {
            if (!rules.anywhere)
                return fail(0, "an absolute name needs names outside folders, which this build of FMinus leaves out "
                               "(FMINUS_FILES_ANYWHERE=OFF): a name begins with a folder its host maps, as "
                               + std::string(example));
            if (rules.windows != drive)
                return fail(0, rules.windows ? "an absolute name on Windows begins with a drive letter, ':' and '/', "
                                               "as \"C:/Samples/kick.wav\""
                                             : "an absolute name begins with '/', as \"/home/me/kick.wav\"");
            if (rules.writes)
                return fail(0, "a file is written only in a folder its host grants for writing, never by an absolute "
                               "name");
            root = drive ? 3 : 1;
        }

        // The segments: [begin, end) of `text`, each the bytes between two '/'.
        std::vector<std::pair<std::size_t, std::size_t>> segments;
        for (std::size_t start = root;;)
        {
            const std::size_t end = std::min(text.find('/', start), text.size());
            if (end == start)
            {
                if (start == 0)
                    return fail(0, std::string("a file's name begins with a folder its host maps, as ") + example);
                return fail(at(end < text.size() ? end : start - 1),
                            "a file's name has no empty segment: no '//', and no '/' at its end");
            }
            segments.emplace_back(start, end);
            if (segments.size() > kMostSegments)
                return fail(at(start - 1), "a file's name has at most 16 segments, the folder's among them");
            if (end == text.size())
                break;
            start = end + 1;
        }
        if (root == 0 && segments.size() < 2)
            return fail(at(segments[0].second < text.size() ? segments[0].second : 0),
                        std::string("a file's name is a folder its host maps, '/' and a path inside it, as ")
                            + example);

        // The folder: a policy's folder's name (1 to 64 ASCII letters, digits, '_' and '-', not beginning with '-').
        std::size_t first = 0;
        if (root == 0)
        {
            const auto [begin, end] = segments[0];
            for (std::size_t k = begin; k < end; ++k)
            {
                if (rules.holes && name.places[k])
                    return fail(at(k), "a file's folder is its literal's own, a name its host maps: no hole in it");
                if (!letterOrDigit(text[k]) && text[k] != '_' && text[k] != '-')
                    return fail(at(k), "character '" + shown(text[k]) + "' is not allowed in a folder's name: ASCII "
                                       "letters, digits, '_' and '-'");
            }
            if (text[begin] == '-')
                return fail(at(begin), "a folder's name does not begin with '-'");
            if (end - begin > kMostFolderName)
                return fail(at(begin), "a folder's name is at most 64 bytes");
            first = 1;
        }

        const std::size_t file = segments.size() - 1;
        for (std::size_t s = first; s < segments.size(); ++s)
        {
            const auto [begin, end] = segments[s];
            const std::string_view segment = std::string_view(text).substr(begin, end - begin);
            if (segment.size() > kMostSegmentBytes)
                return fail(at(begin), "a segment of a file's name is at most 255 bytes");
            for (std::size_t k = begin; k < end;)
            {
                std::size_t length = 1;
                const long cp = codePoint(text, k, length);
                if (cp < 0)
                    return fail(at(k), "illegal character encoding in a file's name, byte '" + shown(text[k])
                                           + "': a name is UTF-8");
                const char c = text[k];
                if (c == '*' && rules.pattern && s == file)
                {
                    k += length;
                    continue;   // the listing's: below
                }
                if (c == '*' && rules.pattern)
                    return fail(at(k), "a listing's '*' is in its file part, the name's last segment");
                const bool forbidden = std::string_view("\\:*?\"<>|").find(c) != std::string_view::npos;
                if (cp < 0x20 || (cp >= 0x7f && cp <= 0x9f) || forbidden)
                    return fail(at(k), "character '"
                                           + (cp < 0x80 ? shown(c) : shownText(segment.substr(k - begin, length)))
                                           + "' is not allowed in a file's name: no control character, and none of "
                                             "\\ : * ? \" < > |");
                k += length;
            }
            if (segment.front() == '.')
                return fail(at(begin), "a segment of a file's name does not begin with '.': '.' and '..' and hidden "
                                       "files are out of reach");
            if (segment.front() == ' ')
                return fail(at(begin), "a segment of a file's name does not begin with a space");
            if (segment.back() == '.' || segment.back() == ' ')
                return fail(at(end - 1), "a segment of a file's name does not end with '.' or a space, which Windows "
                                         "would take away");
            const std::size_t dot = segment.find('.');
            const std::string_view base = segment.substr(0, dot);
            const bool placed = [&] {
                for (std::size_t k = begin; k < begin + base.size(); ++k)
                {
                    if (name.places[k])
                        return true;
                }
                return false;
            }();
            const std::string_view source = literal.substr(at(begin), at(begin + base.size()) - at(begin));
            if (deviceName(base))
                return fail(at(begin), (rules.holes ? std::string("this segment") : "'" + shownText(source) + "'")
                                           + " is a device name of Windows" + (placed ? " when its %d is a digit" : "")
                                           + ": no segment of a file's name may be one, in any letter case, with or "
                                             "without an extension");
        }

        // The file: its extension, written by the literal, and a type this build can touch; a listing's '*'.
        const auto [begin, end] = segments[file];
        const std::string_view segment = std::string_view(text).substr(begin, end - begin);
        const std::size_t dot = segment.rfind('.');
        if (rules.pattern)
        {
            const std::size_t star = segment.find('*');
            if (star == std::string_view::npos)
                return fail(at(begin), "a listing's pattern has one '*' in its file part, as \"kit/*.f32\"");
            if (const std::size_t second = segment.find('*', star + 1); second != std::string_view::npos)
                return fail(at(begin + second), "a listing's pattern has one '*'");
            if (dot == std::string_view::npos || dot < star)
                return fail(at(begin + star), "a listing's '*' comes before its extension, which the pattern writes, "
                                              "as \"kit/*.f32\"");
        }
        if (dot != std::string_view::npos)
        {
            for (std::size_t k = begin + dot + 1; k < end; ++k)
            {
                if (name.places[k])
                    return fail(at(k), "a file's extension is its type, which the literal writes: no %d in it");
            }
        }
        const std::string_view extension = dot == std::string_view::npos ? std::string_view{} : segment.substr(dot + 1);
        if (dot == std::string_view::npos || extension.empty() || extension.size() > kMostExtension
            || !std::ranges::all_of(extension, letterOrDigit))
            return fail(at(dot == std::string_view::npos ? begin : begin + dot),
                        "a file's name ends with its type, an extension: '.' and 1 to 16 ASCII letters and digits, as "
                        "\"7.json\"");
        if (!rules.types.empty())
        {
            bool listed = false;
            for (std::size_t from = 0; from <= rules.types.size() && !listed;)
            {
                const std::size_t to = std::min(rules.types.find(';', from), rules.types.size());
                listed = sameLetters(rules.types.substr(from, to - from), extension);
                from = to + 1;
            }
            if (!listed)
                return fail(at(begin + dot + 1), "'" + std::string(extension) + "' is not a type this build of FMinus "
                                                 "can touch (FMINUS_FILES_TYPES: " + std::string(rules.types) + ")");
        }

        // Its length, each %d at its least, one digit: more is a name that is never good.
        if (text.size() > kMostNameBytes)
            return fail(0, "a file's name is at most 1024 bytes once its %d are filled in");
        return errors;
    }

    // A name, or a listing's pattern: its errors (scanFileName), the first of them at its byte, and then false.
    bool Lowering::checkFileName(const ast::StringExpr& literal, SourceLocation where, std::string_view builtin,
                                 bool holes)
    {
        FileNameRules rules;
        rules.pattern = builtin == "file_list";
        rules.writes = builtin == "file_write" || builtin == "file_append";
        rules.anywhere = FMINUS_HAS_FILES_ANYWHERE != 0;
        const std::string triple = targetTriple_.empty() ? llvm::sys::getProcessTriple() : targetTriple_;
        rules.windows = llvm::Triple(triple).isOSWindows();
        rules.types = FMINUS_FILES_TYPES;
        rules.holes = holes;
        const std::vector<FormatScan::Error> errors = scanFileName(literal.value, rules);
        if (errors.empty())
            return true;
        FormatScan::Error first = errors.front();
        if (holes)   // its places are holes: the rules' words about a %d said of a hole
        {
            if (const std::size_t at = first.message.find("its %d are"); at != std::string::npos)
                first.message.replace(at, 10, "its holes are");
            for (std::size_t at = first.message.find("%d"); at != std::string::npos; at = first.message.find("%d"))
                first.message.replace(at, 2, "hole");
        }
        error(first.offset < literal.at.size() ? literal.at[first.offset] : where, first.message);
        return false;
    }

    // The call's operation and its name as a pattern, each %d (and each hole: a %d or a %s) a "*" and %% a "%", where
    // it is: the module's named metadata, which ir::files reads.
    void Lowering::recordFileUse(SourceLocation location, std::string_view operation, const ast::StringExpr& literal)
    {
        if (relowering_ > 0)
            return;   // a deferred statement lowered again: recorded the first time (Statements.cpp)
        std::string pattern;
        const std::string& text = literal.value;
        for (std::size_t i = 0; i < text.size(); ++i)
        {
            if (text[i] == '%' && i + 1 < text.size())   // the format is checked: %d, %s (a hole's) or %%
            {
                pattern += text[i + 1] == '%' ? '%' : '*';
                ++i;
            }
            else
                pattern += text[i];
        }
        llvm::LLVMContext& context = module_.getContext();
        llvm::Type* i32 = llvm::Type::getInt32Ty(context);
        llvm::NamedMDNode* uses = module_.getOrInsertNamedMetadata(kFilesMetadata);
        uses->addOperand(llvm::MDTuple::get(
            context, {llvm::MDString::get(context, operation), llvm::MDString::get(context, pattern),
                      llvm::MDString::get(context, diagnostics_.file(location.file).name),
                      llvm::ConstantAsMetadata::get(llvm::ConstantInt::getSigned(i32, location.line)),
                      llvm::ConstantAsMetadata::get(llvm::ConstantInt::getSigned(i32, location.column))}));
    }

    // The program's file context, loaded where a name is used: the module's global, defined here at its first use,
    // null until the host sets it. The host sets it once, before it calls anything (Program::compile, an executable's C
    // main), so a plain load reads it. No function or constant of the program can take its name (Functions.cpp and
    // Constants.cpp refuse every name of the file interface's), so the global is the module's only one of that name.
    llvm::Value* Lowering::filesContext()
    {
        llvm::GlobalVariable* global = module_.getNamedGlobal(kFilesContextSymbol);
        if (global == nullptr)
        {
            global = new llvm::GlobalVariable(module_, builder_.getPtrTy(), false, llvm::GlobalValue::ExternalLinkage,
                                              llvm::ConstantPointerNull::get(builder_.getPtrTy()), kFilesContextSymbol);
            global->setAlignment(llvm::Align(8));
        }
        return builder_.CreateAlignedLoad(builder_.getPtrTy(), global, llvm::Align(8), "files.context");
    }

    // The status code `name` names, in a build with the file interface, as an enumeration constant's binding; nullptr
    // for any other name, and in a build without it.
    const Lowering::Binding* Lowering::fileStatus(const std::string& name) const
    {
        if (FMINUS_HAS_FILES == 0)
            return nullptr;
        static const std::array<Binding, kFileStatuses.size()> kBindings = [] {
            std::array<Binding, kFileStatuses.size()> bindings{};
            for (std::size_t i = 0; i < kFileStatuses.size(); ++i)
                bindings[i] = Binding{nullptr, true, ast::Type::Int, {}, kFileStatuses[i].value};
            return bindings;
        }();
        for (std::size_t i = 0; i < kFileStatuses.size(); ++i)
        {
            if (kFileStatuses[i].name == name)
                return &kBindings[i];
        }
        return nullptr;
    }

    // In a build without the file interface, a use of a status code's name is the error that names the option: true
    // when it reported it.
    bool Lowering::unbuiltFileStatus(SourceLocation location, const std::string& name)
    {
        if (isBuilt(Feature::Files))
            return false;
        for (const FileStatus& status : kFileStatuses)
        {
            if (status.name == name)
            {
                error(location, notBuilt(name, Feature::Files));
                return true;
            }
        }
        return false;
    }
} // namespace fminus::ir::detail

namespace fminus::ir
{
    std::vector<FileUse> files(const llvm::Module& module)
    {
        std::vector<FileUse> uses;
        const llvm::NamedMDNode* named = module.getNamedMetadata(detail::kFilesMetadata);
        if (named == nullptr)
            return uses;
        const auto text = [](const llvm::MDNode& node, unsigned i) {
            return llvm::cast<llvm::MDString>(node.getOperand(i))->getString().str();
        };
        const auto number = [](const llvm::MDNode& node, unsigned i) {
            return static_cast<int>(llvm::mdconst::extract<llvm::ConstantInt>(node.getOperand(i))->getSExtValue());
        };
        for (const llvm::MDNode* node : named->operands())
            uses.push_back({text(*node, 0), text(*node, 1), text(*node, 2), number(*node, 3), number(*node, 4)});
        return uses;
    }
} // namespace fminus::ir
