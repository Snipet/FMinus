// src/runtime/Files.cpp: the file interface (Files.h, and Runtime.h's fminus_rt_file_*), compiled only into a build
// with FMINUS_FILES: names checked in full, policies checked against the build's ceilings, contexts and their quotas,
// the handles a program holds, and an executable's context from its environment. It opens nothing itself: the system's
// part is FilesSystem.h's (FilesPosix.cpp, FilesWindows.cpp), reached for every operation through one function,
// Context::State::reach, which checks a name and walks to its directory.
//
// Reading a handle (status, size, data, count) reads fields that never change once the handle is returned: no
// allocation, no lock, no system call. Everything else allocates, and most of it waits on the disk.
#include "runtime/Files.h"

#include "runtime/Runtime.h"

#if FMINUS_HAS_FILES
#include "runtime/FilesSystem.h"
#include "runtime/Format.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fminus::files
{
    namespace
    {
        constexpr std::size_t kMostNameBytes = 1024;
        constexpr std::size_t kMostSegments = 16;
        constexpr std::size_t kMostSegmentBytes = 255;
        constexpr std::size_t kMostExtension = 16;
        constexpr std::size_t kMostFolderName = 64;

        char lower(char c) noexcept
        {
            return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
        }

        bool letterOrDigit(char c) noexcept
        {
            return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        }

        bool sameLetters(std::string_view a, std::string_view b) noexcept
        {
            if (a.size() != b.size())
                return false;
            for (std::size_t i = 0; i < a.size(); ++i)
                if (lower(a[i]) != lower(b[i]))
                    return false;
            return true;
        }

        std::string quoted(std::string_view text)
        {
            return "'" + std::string(text) + "'";
        }

        // An extension as a policy or the build writes one: 1 to 16 ASCII letters and digits.
        bool validExtension(std::string_view ext) noexcept
        {
            return !ext.empty() && ext.size() <= kMostExtension && std::ranges::all_of(ext, letterOrDigit);
        }

        // The build's types (FMINUS_FILES_TYPES), in lower case; none: any.
        const std::vector<std::string>& buildTypes()
        {
            static const std::vector<std::string> types = []
            {
                std::vector<std::string> out;
                const std::string_view all = FMINUS_FILES_TYPES;
                std::size_t at = 0;
                while (at <= all.size())
                {
                    const std::size_t end = std::min(all.find(';', at), all.size());
                    if (end > at)
                        out.emplace_back(all.substr(at, end - at));
                    at = end + 1;
                }
                return out;
            }();
            return types;
        }

        bool listed(const std::vector<std::string>& types, std::string_view ext) noexcept
        {
            return std::ranges::any_of(types, [&](const std::string& t) { return sameLetters(t, ext); });
        }

        // The code point UTF-8 starts at `s[i]` (advancing `i`), or -1 when it is not well formed: overlong, a
        // surrogate, past U+10FFFF, or cut short.
        long nextCodePoint(std::string_view s, std::size_t& i) noexcept
        {
            const auto byte = [&](std::size_t k)
            {
                return static_cast<unsigned char>(s[k]);
            };
            const unsigned char b0 = byte(i);
            if (b0 < 0x80)
            {
                ++i;
                return b0;
            }
            int extra = 0;
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
            if (i + static_cast<std::size_t>(extra) >= s.size())
                return -1;   // cut short
            for (int k = 1; k <= extra; ++k)
            {
                const unsigned char b = byte(i + static_cast<std::size_t>(k));
                if ((b & 0xc0) != 0x80)
                    return -1;
                cp = (cp << 6) | (b & 0x3f);
            }
            if (cp < least || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
                return -1;
            i += static_cast<std::size_t>(extra) + 1;
            return cp;
        }

        // Whether `segment`, spaces at its end aside, before its first ".", is a device name of Windows.
        bool deviceName(std::string_view segment) noexcept
        {
            std::string_view base = segment.substr(0, segment.find('.'));
            while (!base.empty() && base.back() == ' ')
                base.remove_suffix(1);
            for (const std::string_view device : {"CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$"})
                if (sameLetters(base, device))
                    return true;
            if (base.size() < 4 || (!sameLetters(base.substr(0, 3), "COM") && !sameLetters(base.substr(0, 3), "LPT")))
                return false;
            const std::string_view number = base.substr(3);
            if (number.size() == 1)
                return number[0] >= '0' && number[0] <= '9';
            return number == "\xc2\xb9" || number == "\xc2\xb2" || number == "\xc2\xb3";   // superscript 1, 2, 3
        }

        // Whether `segment` is one a name may have (Files.h): its bytes, its ends, and not a device's name.
        bool validSegment(std::string_view segment) noexcept
        {
            if (segment.empty() || segment.size() > kMostSegmentBytes)
                return false;
            if (segment.front() == '.' || segment.front() == ' ' || segment.back() == '.' || segment.back() == ' ')
                return false;
            for (std::size_t i = 0; i < segment.size();)
            {
                const long cp = nextCodePoint(segment, i);
                if (cp < 0x20 || (cp >= 0x7f && cp <= 0x9f))
                    return false;   // not UTF-8 (-1), or a control character
                if (cp < 0x80 && std::string_view("/\\:*?\"<>|").find(static_cast<char>(cp)) != std::string_view::npos)
                    return false;
            }
            return !deviceName(segment);
        }

        // A file segment's extension: what follows its last ".", when it is 1 to 16 ASCII letters and digits; else
        // empty (no extension, which a file must have).
        std::string_view extensionOf(std::string_view file) noexcept
        {
            const std::size_t dot = file.rfind('.');
            if (dot == std::string_view::npos || dot == 0)
                return {};
            const std::string_view ext = file.substr(dot + 1);
            return validExtension(ext) ? ext : std::string_view{};
        }

        // A folder's name, as a policy may give one.
        bool validFolderName(std::string_view name) noexcept
        {
            return !name.empty() && name.size() <= kMostFolderName && name.front() != '-' &&
                   std::ranges::all_of(name, [](char c) { return letterOrDigit(c) || c == '_' || c == '-'; });
        }

        // Whether the format's file part writes its extension: no %-sequence after the last "." of what follows the
        // last "/" (a %d there could make any type).
        bool extensionWritten(std::string_view format) noexcept
        {
            const std::size_t slash = format.rfind('/');
            const std::string_view file = slash == std::string_view::npos ? format : format.substr(slash + 1);
            const std::size_t dot = file.rfind('.');
            return dot != std::string_view::npos && file.substr(dot + 1).find('%') == std::string_view::npos;
        }

        // A name, checked and split (Files.h): absolute, or a folder's; its directories; its file (or pattern).
        struct Name
        {
            bool absolute = false;
            std::string_view folder;                     // the first segment, when not absolute
            std::vector<std::string_view> directories;   // the segments between the folder (or the root) and the file
            std::string_view file;                       // the last segment
            std::string_view extension;
            std::string_view before, after;   // a pattern's fixed parts, either side of its "*"
            std::size_t directoryBytes = 0;   // the bytes of the text before the file: "kit/sub/", "/a/b/"
        };

        // Splits `text` (its %d filled in) into `out`: FILE_OK, or FILE_REFUSED for any name the rules refuse.
        std::int32_t parseName(std::string_view text, bool pattern, Name& out)
        {
            if (text.empty() || text.size() > kMostNameBytes)
                return FMINUS_FILE_REFUSED;
            const std::size_t root = os::absolutePrefix(text);
            out.absolute = root > 0;
            std::vector<std::string_view> segments;
            std::size_t at = root;
            while (true)
            {
                const std::size_t slash = text.find('/', at);
                const std::string_view segment =
                    text.substr(at, slash == std::string_view::npos ? text.npos : slash - at);
                if (segment.empty())
                    return FMINUS_FILE_REFUSED;   // "//", a "/" at the end, or nothing after the root
                segments.push_back(segment);
                if (segments.size() > kMostSegments)
                    return FMINUS_FILE_REFUSED;
                if (slash == std::string_view::npos)
                    break;
                at = slash + 1;
            }
            if (!out.absolute && segments.size() < 2)
                return FMINUS_FILE_REFUSED;   // a folder, then at least a file
            std::size_t first = 0;
            if (!out.absolute)
                out.folder = segments[first++];
            for (std::size_t i = first; i + 1 < segments.size(); ++i)
            {
                if (!validSegment(segments[i]))
                    return FMINUS_FILE_REFUSED;
                out.directories.push_back(segments[i]);
            }
            out.file = segments.back();
            out.directoryBytes = text.size() - out.file.size();
            if (pattern)
            {
                const std::size_t star = out.file.find('*');
                if (star == std::string_view::npos || out.file.find('*', star + 1) != std::string_view::npos)
                    return FMINUS_FILE_REFUSED;
                const std::size_t dot = out.file.rfind('.');
                if (dot == std::string_view::npos || dot < star)
                    return FMINUS_FILE_REFUSED;   // the "*" before the extension, which is written
                std::string stand(out.file);
                stand[star] = 'x';
                if (!validSegment(stand) || extensionOf(stand).empty())
                    return FMINUS_FILE_REFUSED;
                out.before = out.file.substr(0, star);
                out.after = out.file.substr(star + 1);
                out.extension = extensionOf(out.file);
                return FMINUS_FILE_OK;
            }
            if (!validSegment(out.file))
                return FMINUS_FILE_REFUSED;
            out.extension = extensionOf(out.file);
            return out.extension.empty() ? FMINUS_FILE_REFUSED : FMINUS_FILE_OK;
        }

        // Whether a pattern's fixed parts fit `name`, ASCII letter case aside.
        bool fits(const Name& pattern, std::string_view name) noexcept
        {
            return name.size() >= pattern.before.size() + pattern.after.size() &&
                   sameLetters(name.substr(0, pattern.before.size()), pattern.before) &&
                   sameLetters(name.substr(name.size() - pattern.after.size()), pattern.after);
        }

        // The text a format and its ints make, in `out` (at most 1024 bytes and one more, to tell a longer one).
        std::string_view fill(const char* format, std::int32_t length, const std::int32_t* ints, std::int32_t count,
                              char (&out)[kMostNameBytes + 1], bool& tooLong) noexcept
        {
            std::size_t n = 0;
            tooLong = false;
            runtime::expandFormat(format, length, ints, count,
                                  [&](char c)
                                  {
                                      if (n < sizeof out)
                                          out[n++] = c;
                                      else
                                          tooLong = true;
                                  });
            if (n > kMostNameBytes)
                tooLong = true;
            return std::string_view(out, n);
        }

        enum class Op
        {
            Read,
            List,
            Write,
            Append,
        };

        // The name a builtin was given, its %d filled in, in `buffer`: false when it is longer than a name may be, or
        // its literal's extension is not written out (a %d after its file part's last ".").
        bool prepare(const char* format, std::int32_t length, const std::int32_t* ints, std::int32_t count,
                     char (&buffer)[kMostNameBytes + 1], std::string_view& text) noexcept
        {
            bool tooLong = false;
            text = fill(format, length, ints, count, buffer, tooLong);
            const std::string_view literal = format != nullptr && length > 0
                                                 ? std::string_view(format, static_cast<std::size_t>(length))
                                                 : std::string_view{};
            return !tooLong && extensionWritten(literal);
        }
    }   // namespace

    // What every handle begins with: its kind, so that a handle of another kind is never read as this one. The kinds
    // are distinct 32-bit words, as the web library's are (web/Handle.h), so that memory that was never a handle is
    // unlikely to pass for one.
    enum class Kind : std::uint32_t
    {
        File = 0x464d4666,     // "FMFf": a file read (or one that could not be)
        List = 0x464d466c,     // "FMFl": a listing
        Closed = 0x464d4678,   // "FMFx": a handle closed, its place kept by its context for the next one
    };

    struct ListData
    {
        std::string directory;            // the text before each file's name: "kit/sub/", "/a/b/"
        std::vector<std::string> names;   // sorted by their bytes
    };

    // A handle. The first four fields are what the wait-free functions read, and never change once it is returned
    // until it is closed; the rest is the context's. A handle with no owner is one of the shared ones that say why
    // something failed, or a closed one. A closed handle's memory stays its context's (reused for a later handle, freed
    // with the context), so a second close, or a read of a closed handle, finds Kind::Closed whatever the allocator.
    struct Handle
    {
        Kind kind;
        std::int32_t status;
        std::int32_t size;   // a file's bytes, a listing's files
        const void* data;    // a file's bytes (one 0 byte after the last), else null
        Context::State* owner;
        Handle* previous;
        Handle* next;
        ListData* list;
    };

    namespace
    {
        // The shared handles of failures, one per kind and status: never freed, never counted, closed harmlessly.
        constinit Handle failures[2][6] = {
            {{Kind::File, FMINUS_FILE_MISSING, 0, nullptr, nullptr, nullptr, nullptr, nullptr},
             {Kind::File, FMINUS_FILE_REFUSED, 0, nullptr, nullptr, nullptr, nullptr, nullptr},
             {Kind::File, FMINUS_FILE_DENIED, 0, nullptr, nullptr, nullptr, nullptr, nullptr},
             {Kind::File, FMINUS_FILE_LIMIT, 0, nullptr, nullptr, nullptr, nullptr, nullptr},
             {Kind::File, FMINUS_FILE_FAILED, 0, nullptr, nullptr, nullptr, nullptr, nullptr},
             {Kind::File, FMINUS_FILE_INVALID, 0, nullptr, nullptr, nullptr, nullptr, nullptr}},
            {{Kind::List, FMINUS_FILE_MISSING, 0, nullptr, nullptr, nullptr, nullptr, nullptr},
             {Kind::List, FMINUS_FILE_REFUSED, 0, nullptr, nullptr, nullptr, nullptr, nullptr},
             {Kind::List, FMINUS_FILE_DENIED, 0, nullptr, nullptr, nullptr, nullptr, nullptr},
             {Kind::List, FMINUS_FILE_LIMIT, 0, nullptr, nullptr, nullptr, nullptr, nullptr},
             {Kind::List, FMINUS_FILE_FAILED, 0, nullptr, nullptr, nullptr, nullptr, nullptr},
             {Kind::List, FMINUS_FILE_INVALID, 0, nullptr, nullptr, nullptr, nullptr, nullptr}},
        };

        Handle* failure(Kind kind, std::int32_t status) noexcept
        {
            const int row = kind == Kind::File ? 0 : 1;
            const int column = status <= FMINUS_FILE_INVALID || status >= 0 ? 5 : -status - 1;
            return &failures[row][column];
        }

        const Handle* as(const void* handle, Kind kind) noexcept
        {
            const auto* h = static_cast<const Handle*>(handle);
            return h != nullptr && h->kind == kind ? h : nullptr;
        }

        const Handle* anyHandle(const void* handle) noexcept
        {
            const auto* h = static_cast<const Handle*>(handle);
            return h != nullptr && (h->kind == Kind::File || h->kind == Kind::List) ? h : nullptr;
        }
    }   // namespace

    struct Context::State
    {
        struct Folder
        {
            std::string name;
            os::Dir directory;
            bool write = false;
            std::vector<std::string> types;   // lower case; empty: the policy's
        };
        struct Denied
        {
            std::string path;
            os::Dir pinned;   // what was there when the context was made, if anything
            os::Identity identity;
        };

        // What one look at a denied path found there.
        struct Look
        {
            bool present = false;
            os::Identity identity;
        };

        // The most identities remembered from the denied paths beyond those pinned (see seen): past it, every
        // operation is refused, as a deny-list that cannot be kept is closed.
        static constexpr std::size_t kMostSeen = 4096;

        Policy policy;
        std::vector<Folder> folders;
        std::vector<Denied> denied;
        std::vector<std::string> types;   // the policy's, lower case; empty: the build's (or any)

        mutable std::mutex mutex;
        Handle* first = nullptr;
        Handle* spare = nullptr;   // closed handles, their places kept for the next ones
        std::int32_t held = 0;
        std::int64_t heldBytes = 0;
        std::int64_t written = 0;
        std::int32_t filesWritten = 0;
        // Every identity found at a denied path since the context was made, besides those pinned then: a directory
        // made at a denied path afterwards is denied from the first look that finds it there, wherever it goes next.
        mutable std::vector<os::Identity> seen;
        mutable bool seenTooMany = false;
#if FMINUS_HAS_FILES_WRITE
        std::vector<os::Identity> swept;   // directories whose stale temporary files this context has removed
#endif

        // Where a checked name's file lies: its directory, opened; the identities no step may be; what the denied
        // paths were when the walk began.
        struct Reached
        {
            os::Dir directory;           // the last directory walked to (empty: the folder's own)
            os::Native at = os::kNone;   // the directory the file is in
            std::vector<os::Identity> denied;
            std::vector<Look> looks;   // one per denied path
        };

        // Remembers `id`, found at a denied path (the mutex held).
        void remember(const os::Identity& id) const
        {
            for (const Denied& d : denied)
                if (d.pinned.valid() && d.identity == id)
                    return;
            if (std::ranges::find(seen, id) != seen.end())
                return;
            if (seen.size() >= kMostSeen)
                seenTooMany = true;
            else
                seen.push_back(id);
        }

        // Looks at every denied path now, one Look each, and remembers what is found: FILE_OK, or FILE_REFUSED when
        // one cannot be looked up for a reason other than its absence (the deny-list fails closed), or when more has
        // been found at them than can be remembered.
        std::int32_t lookAtDenied(std::vector<Look>& looks) const
        {
            looks.clear();
            for (const Denied& d : denied)
            {
                Look look;
                const std::int32_t status = os::lookUp(d.path, look.identity);
                if (status == FMINUS_FILE_OK)
                    look.present = true;
                else if (status != FMINUS_FILE_MISSING)
                    return FMINUS_FILE_REFUSED;
                looks.push_back(look);
            }
            const std::lock_guard lock(mutex);
            for (const Look& look : looks)
                if (look.present)
                    remember(look.identity);
            return seenTooMany ? FMINUS_FILE_REFUSED : FMINUS_FILE_OK;
        }

        // Whether every denied path is as the operation found it when it began: FILE_OK, or FILE_REFUSED when what is
        // at one changed meanwhile (what is there now is remembered too). A directory swapped in at a denied path, or
        // out of one, while a name was walked would otherwise pass unseen. Looked at once a walk is done, and again
        // once a file is read or a directory listed.
        std::int32_t settled(const Reached& reached) const
        {
            if (denied.empty())
                return FMINUS_FILE_OK;
            std::vector<Look> now;
            if (const std::int32_t status = lookAtDenied(now); status != FMINUS_FILE_OK)
                return status;
            for (std::size_t i = 0; i < now.size(); ++i)
                if (now[i].present != reached.looks[i].present ||
                    (now[i].present && !(now[i].identity == reached.looks[i].identity)))
                    return FMINUS_FILE_REFUSED;
            return FMINUS_FILE_OK;
        }

        // The one function every operation goes through: checks `text` (a name, or with `pattern` a listing's) in
        // full against the rules and the policy for `op`, then walks to its directory, every step and the folder's
        // ancestors compared with the denied directories.
        std::int32_t reach(Op op, std::string_view text, bool pattern, Name& name, Reached& out) const
        {
            if (parseName(text, pattern, name) != FMINUS_FILE_OK)
                return FMINUS_FILE_REFUSED;
            const Folder* folder = nullptr;
            if (name.absolute)
            {
                if (!FMINUS_HAS_FILES_ANYWHERE || !policy.anywhere || op == Op::Write || op == Op::Append)
                    return FMINUS_FILE_REFUSED;
            }
            else
            {
                for (const Folder& f : folders)
                    if (f.name == name.folder)
                        folder = &f;
                if (folder == nullptr)
                    return FMINUS_FILE_REFUSED;
                if ((op == Op::Write || op == Op::Append) && !folder->write)
                    return FMINUS_FILE_REFUSED;
            }
            if (!typeAllowed(folder, name.extension))
                return FMINUS_FILE_REFUSED;

            // The denied directories as they are now: those pinned when the context was made, whatever is at each
            // path now, and whatever was ever found at one. One that cannot be looked up (but is not simply absent)
            // closes everything.
            if (!denied.empty())
            {
                if (const std::int32_t status = lookAtDenied(out.looks); status != FMINUS_FILE_OK)
                    return status;
                const std::lock_guard lock(mutex);
                for (const Denied& d : denied)
                    if (d.pinned.valid())
                        out.denied.push_back(d.identity);
                out.denied.insert(out.denied.end(), seen.begin(), seen.end());
            }
            const auto deniedOne = [&](os::Native native) -> std::int32_t
            {
                if (out.denied.empty())
                    return FMINUS_FILE_OK;
                os::Identity id;
                if (const std::int32_t status = os::identityOf(native, id); status != FMINUS_FILE_OK)
                    return status;
                return std::ranges::find(out.denied, id) == out.denied.end() ? FMINUS_FILE_OK : FMINUS_FILE_REFUSED;
            };

            os::Dir current;
            os::Native at = os::kNone;
            std::vector<std::string_view> walk;
#if FMINUS_HAS_FILES_ANYWHERE
            std::string real;
            std::vector<std::string_view> realSegments;
#endif
            if (folder != nullptr)
            {
                at = folder->directory.get();
                if (const std::int32_t status = deniedOne(at); status != FMINUS_FILE_OK)
                    return status;
                if (!out.denied.empty())
                {
                    std::vector<os::Identity> above;
                    if (const std::int32_t status = os::ancestors(at, above); status != FMINUS_FILE_OK)
                        return status == FMINUS_FILE_DENIED ? status : FMINUS_FILE_REFUSED;
                    for (const os::Identity& id : above)
                        if (std::ranges::find(out.denied, id) != out.denied.end())
                            return FMINUS_FILE_REFUSED;
                }
                walk = name.directories;
            }
            else
            {
#if FMINUS_HAS_FILES_ANYWHERE
                // Anywhere: the directory's links followed, to find the real one; its path then walked from the root,
                // under the same rules, as if a program had written it.
                const std::string directory(text.substr(0, name.directoryBytes));
                if (const std::int32_t status = os::realDirectory(directory, real); status != FMINUS_FILE_OK)
                    return status;
                std::size_t rest = 0;
                if (const std::int32_t status = os::openRoot(real, current, rest); status != FMINUS_FILE_OK)
                    return status;
                at = current.get();
                if (const std::int32_t status = deniedOne(at); status != FMINUS_FILE_OK)
                    return status;
                std::string_view remaining = std::string_view(real).substr(rest);
                while (!remaining.empty())
                {
                    const std::size_t slash = remaining.find('/');
                    const std::string_view segment = remaining.substr(0, slash);
                    if (!validSegment(segment))
                        return FMINUS_FILE_REFUSED;
                    realSegments.push_back(segment);
                    remaining = slash == std::string_view::npos ? std::string_view{} : remaining.substr(slash + 1);
                }
                if (real.size() + name.file.size() + 1 > kMostNameBytes || realSegments.size() + 1 > kMostSegments)
                    return FMINUS_FILE_REFUSED;
                walk = realSegments;
#else
                return FMINUS_FILE_REFUSED;   // never reached: an absolute name was refused above
#endif
            }
            for (const std::string_view segment : walk)
            {
                os::Dir next;
                if (const std::int32_t status = os::openDirectory(at, segment, next); status != FMINUS_FILE_OK)
                    return status;
                if (const std::int32_t status = deniedOne(next.get()); status != FMINUS_FILE_OK)
                    return status;
                current = std::move(next);
                at = current.get();
            }
            out.directory = std::move(current);
            out.at = at;
            return settled(out);
        }

        // Whether a file of type `ext` may be touched in `folder` (null: an absolute name): the folder's list, else
        // the policy's, else the build's; and always the build's.
        bool typeAllowed(const Folder* folder, std::string_view ext) const
        {
            if (ext.empty())
                return false;
            if (!buildTypes().empty() && !listed(buildTypes(), ext))
                return false;
            if (folder != nullptr && !folder->types.empty())
                return listed(folder->types, ext);
            if (!types.empty())
                return listed(types, ext);
            return true;
        }

        // Takes one of maxOpen's places for a handle about to be made: false when all are taken.
        bool takePlace()
        {
            const std::lock_guard lock(mutex);
            if (held >= policy.maxOpen)
                return false;
            ++held;
            return true;
        }

        void givePlace(std::int64_t bytes)
        {
            const std::lock_guard lock(mutex);
            --held;
            heldBytes -= bytes;
        }

        // A new handle of `kind` holding `size`, `data` and `list`, linked into the context's list (its place already
        // taken): a closed one's memory when there is one, else new memory; null when there is none.
        Handle* adopt(Kind kind, std::int32_t size, const void* data, ListData* list)
        {
            const std::lock_guard lock(mutex);
            Handle* handle = spare;
            if (handle != nullptr)
                spare = handle->next;
            else
                handle = new (std::nothrow) Handle{};
            if (handle == nullptr)
                return nullptr;
            *handle = Handle{kind, FMINUS_FILE_OK, size, data, this, nullptr, first, list};
            if (first != nullptr)
                first->previous = handle;
            first = handle;
            return handle;
        }

        static void destroy(Handle* handle) noexcept
        {
            std::free(const_cast<void*>(handle->data));
            delete handle->list;
            delete handle;
        }

        void close(Handle* handle)
        {
            const void* data = nullptr;
            ListData* list = nullptr;
            {
                const std::lock_guard lock(mutex);
                if (handle->previous != nullptr)
                    handle->previous->next = handle->next;
                else
                    first = handle->next;
                if (handle->next != nullptr)
                    handle->next->previous = handle->previous;
                --held;
                if (handle->kind == Kind::File)
                    heldBytes -= handle->size;
                data = handle->data;
                list = handle->list;
                *handle = Handle{Kind::Closed, FMINUS_FILE_INVALID, 0, nullptr, nullptr, nullptr, spare, nullptr};
                spare = handle;
            }
            std::free(const_cast<void*>(data));
            delete list;
        }

        // file_read, and file_read_nth with its listing's name.
        Handle* read(std::string_view text)
        {
            Name name;
            Reached reached;
            if (const std::int32_t status = reach(Op::Read, text, false, name, reached); status != FMINUS_FILE_OK)
                return failure(Kind::File, status);
            if (!takePlace())
                return failure(Kind::File, FMINUS_FILE_LIMIT);
            os::ReadRules rules;
            rules.limit = policy.maxFileBytes;
            rules.singleLink = policy.singleLink;
            rules.denied = &reached.denied;
            unsigned char* data = nullptr;
            std::int32_t size = 0;
            std::int32_t status = os::readFile(reached.at, name.file, rules, data, size);
            if (status == FMINUS_FILE_OK)
            {
                status = settled(reached);
                if (status != FMINUS_FILE_OK)
                {
                    std::free(data);
                    data = nullptr;
                }
            }
            if (status != FMINUS_FILE_OK)
            {
                givePlace(0);
                return failure(Kind::File, status);
            }
            {
                const std::lock_guard lock(mutex);
                if (heldBytes + size > policy.maxHeldBytes)
                {
                    --held;
                    std::free(data);
                    return failure(Kind::File, FMINUS_FILE_LIMIT);
                }
                heldBytes += size;
            }
            Handle* handle = adopt(Kind::File, size, data, nullptr);
            if (handle == nullptr)
            {
                std::free(data);
                givePlace(size);
                return failure(Kind::File, FMINUS_FILE_FAILED);
            }
            return handle;
        }

        // file_list.
        Handle* list(std::string_view text)
        {
            Name name;
            Reached reached;
            if (const std::int32_t status = reach(Op::List, text, true, name, reached); status != FMINUS_FILE_OK)
                return failure(Kind::List, status);
            const Folder* folder = nullptr;
            for (const Folder& f : folders)
                if (!name.absolute && f.name == name.folder)
                    folder = &f;
            auto data = std::make_unique<ListData>();
            data->directory = std::string(text.substr(0, name.directoryBytes));
            struct Collect final : os::Lister
            {
                const State& state;
                const Name& pattern;
                const Folder* folder;
                std::vector<std::string>& names;
                Collect(const State& s, const Name& p, const Folder* f, std::vector<std::string>& n)
                    : state(s)
                    , pattern(p)
                    , folder(f)
                    , names(n)
                {
                }
                bool wanted(std::string_view n) override
                {
                    // A name a program could have written here, of a type it may read here.
                    return fits(pattern, n) && validSegment(n) && state.typeAllowed(folder, extensionOf(n)) &&
                           n.size() + pattern.directoryBytes <= kMostNameBytes;
                }
                std::int32_t entry(const os::Entry& e) override
                {
                    if (!e.regular || (state.policy.singleLink && e.links > 1))
                        return FMINUS_FILE_OK;
                    if (names.size() == static_cast<std::size_t>(state.policy.maxListed))
                        return FMINUS_FILE_LIMIT;
                    names.push_back(e.name);
                    return FMINUS_FILE_OK;
                }
            } collect(*this, name, folder, data->names);
            if (!takePlace())
                return failure(Kind::List, FMINUS_FILE_LIMIT);
            std::int32_t status = os::listDirectory(reached.at, collect);
            if (status == FMINUS_FILE_OK)
                status = settled(reached);
            if (status != FMINUS_FILE_OK)
            {
                givePlace(0);
                return failure(Kind::List, status);
            }
            std::ranges::sort(data->names);
            Handle* handle = adopt(Kind::List, static_cast<std::int32_t>(data->names.size()), nullptr, data.get());
            if (handle == nullptr)
            {
                givePlace(0);
                return failure(Kind::List, FMINUS_FILE_FAILED);
            }
            data.release();
            return handle;
        }

#if FMINUS_HAS_FILES_WRITE
        // file_write and file_append.
        std::int32_t write(std::string_view text, const void* data, std::int32_t bytes, bool append)
        {
            if (bytes < 0 || (data == nullptr && bytes > 0))
                return FMINUS_FILE_INVALID;
            Name name;
            Reached reached;
            if (const std::int32_t status = reach(append ? Op::Append : Op::Write, text, false, name, reached);
                status != FMINUS_FILE_OK)
                return status;
            if (bytes > policy.maxFileBytes)
                return FMINUS_FILE_LIMIT;
            // The quotas count what is attempted: bytes and files are never given back.
            bool mayCreate = false;
            {
                const std::lock_guard lock(mutex);
                if (written + bytes > policy.maxWrittenBytes)
                    return FMINUS_FILE_LIMIT;
                mayCreate = filesWritten < policy.maxFilesWritten;
                if (!append && !mayCreate)
                    return FMINUS_FILE_LIMIT;
                written += bytes;
                if (mayCreate)
                    ++filesWritten;   // taken now, and given back below if no file was made
            }
            // The first replacement in a directory removes what earlier ones left there when a process died half way
            // (FilesSystem.h, sweepTemporaries), so their bytes do not pile up across runs.
            if (!append)
            {
                os::Identity here;
                bool sweep = false;
                if (os::identityOf(reached.at, here) == FMINUS_FILE_OK)
                {
                    const std::lock_guard lock(mutex);
                    sweep = swept.size() < kMostSeen && std::ranges::find(swept, here) == swept.end();
                    if (sweep)
                        swept.push_back(here);
                }
                if (sweep)
                    os::sweepTemporaries(reached.at);
            }
            os::WriteRules rules;
            rules.limit = policy.maxFileBytes;
            rules.denied = &reached.denied;
            rules.mayCreate = mayCreate;
            bool created = false;
            const std::int32_t status = append ? os::appendFile(reached.at, name.file, data, bytes, rules, created)
                                               : os::replaceFile(reached.at, name.file, data, bytes, rules, created);
            if (mayCreate && !created)
            {
                const std::lock_guard lock(mutex);
                --filesWritten;
            }
            return status;
        }
#endif

        ~State()
        {
            for (Handle* list : {first, spare})
                while (list != nullptr)
                {
                    Handle* next = list->next;
                    destroy(list);
                    list = next;
                }
        }
    };

    std::string checkPolicy(const Policy& policy)
    {
        const auto ceilingTypes = []
        {
            std::string all;
            for (const std::string& t : buildTypes())
                all += (all.empty() ? "" : ";") + t;
            return all;
        };
        const auto checkTypes = [&](const std::vector<std::string>& types, const std::string& where) -> std::string
        {
            for (const std::string& t : types)
            {
                if (!validExtension(t))
                    return where + "types: " + quoted(t) +
                           " is not an extension: 1 to 16 ASCII letters and digits, without the dot";
                if (!buildTypes().empty() && !listed(buildTypes(), t))
                    return where + "types: " + quoted(t) + " is not a type this build of FMinus can touch " +
                           "(FMINUS_FILES_TYPES: " + ceilingTypes() + ")";
            }
            return {};
        };
        for (std::size_t i = 0; i < policy.folders.size(); ++i)
        {
            const Policy::Folder& folder = policy.folders[i];
            if (!validFolderName(folder.name))
                return "folders: " + quoted(folder.name) +
                       " is not a folder's name: 1 to 64 ASCII letters, digits, '_' and '-', not beginning with '-'";
            for (std::size_t j = 0; j < i; ++j)
                if (sameLetters(policy.folders[j].name, folder.name))
                    return "folders: " + quoted(folder.name) +
                           " is named twice (letter case aside: " + quoted(policy.folders[j].name) + ")";
            const std::string where = "folders: " + quoted(folder.name) + ": ";
            if (folder.directory.empty())
                return where + "its directory is empty";
            if (folder.directory.find('\0') != std::string::npos)
                return where + "its directory has a NUL byte";
            if (folder.write && !FMINUS_HAS_FILES_WRITE)
                return where + "write is set, and this build of FMinus leaves out writing files "
                               "(FMINUS_FILES_WRITE=OFF)";
            if (std::string wrong = checkTypes(folder.types, where); !wrong.empty())
                return wrong;
        }
        if (std::string wrong = checkTypes(policy.types, ""); !wrong.empty())
            return wrong;
        for (const std::string& path : policy.deny)
        {
            if (path.empty())
                return "deny: a path is empty";
            if (path.find('\0') != std::string::npos)
                return "deny: " + quoted(path) + " has a NUL byte";
        }
        if (policy.anywhere && !FMINUS_HAS_FILES_ANYWHERE)
            return "anywhere is set, and this build of FMinus leaves out absolute names (FMINUS_FILES_ANYWHERE=OFF)";
        if (policy.maxFileBytes < 1)
            return "maxFileBytes is " + std::to_string(policy.maxFileBytes) + ": from 1 to " +
                   std::to_string(FMINUS_FILES_MAX_BYTES) + " (FMINUS_FILES_MAX_BYTES)";
        if (policy.maxFileBytes > FMINUS_FILES_MAX_BYTES)
            return "maxFileBytes is " + std::to_string(policy.maxFileBytes) + ", over this build's ceiling of " +
                   std::to_string(FMINUS_FILES_MAX_BYTES) + " (FMINUS_FILES_MAX_BYTES)";
        if (policy.maxOpen < 1 || policy.maxOpen > 65536)
            return "maxOpen is " + std::to_string(policy.maxOpen) + ": from 1 to 65536";
        constexpr std::int64_t most = std::int64_t{1} << 48;
        if (policy.maxHeldBytes < 0 || policy.maxHeldBytes > most)
            return "maxHeldBytes is " + std::to_string(policy.maxHeldBytes) + ": from 0 to 2^48";
        if (policy.maxWrittenBytes < 0 || policy.maxWrittenBytes > most)
            return "maxWrittenBytes is " + std::to_string(policy.maxWrittenBytes) + ": from 0 to 2^48";
        if (policy.maxFilesWritten < 0)
            return "maxFilesWritten is " + std::to_string(policy.maxFilesWritten) + ": from 0 to 2147483647";
        if (policy.maxListed < 1 || policy.maxListed > (1 << 20))
            return "maxListed is " + std::to_string(policy.maxListed) + ": from 1 to 1048576";
        return {};
    }

    std::unique_ptr<Context> Context::create(const Policy& policy, std::string& error)
    {
        error = checkPolicy(policy);
        if (!error.empty())
            return nullptr;
        auto state = std::make_unique<State>();
        state->policy = policy;
        for (const std::string& t : policy.types)
        {
            std::string low;
            for (const char c : t)
                low += lower(c);
            state->types.push_back(low);
        }
        for (const Policy::Folder& folder : policy.folders)
        {
            State::Folder made;
            made.name = folder.name;
            made.write = folder.write;
            for (const std::string& t : folder.types)
            {
                std::string low;
                for (const char c : t)
                    low += lower(c);
                made.types.push_back(low);
            }
            std::string why;
            if (os::openFolder(folder.directory, made.directory, why) != FMINUS_FILE_OK)
            {
                error =
                    "folders: " + quoted(folder.name) + ": " + quoted(folder.directory) + " cannot be opened: " + why;
                return nullptr;
            }
            state->folders.push_back(std::move(made));
        }
        for (const std::string& path : policy.deny)
        {
            State::Denied d;
            d.path = path;
            const std::int32_t status = os::openDenied(path, d.pinned);
            if (status == FMINUS_FILE_OK)
            {
                if (os::identityOf(d.pinned.get(), d.identity) != FMINUS_FILE_OK)
                {
                    error = "deny: " + quoted(path) + " cannot be looked at";
                    return nullptr;
                }
            }
            else if (status != FMINUS_FILE_MISSING)
            {
                // Something is there that cannot be pinned: what it is cannot be known, so nothing could be allowed
                // beside it with certainty.
                error = "deny: " + quoted(path) + " cannot be opened";
                return nullptr;
            }
            state->denied.push_back(std::move(d));
        }
        return std::unique_ptr<Context>(new Context(std::move(state)));
    }

    Context::Context(std::unique_ptr<State> state)
        : state_(std::move(state))
    {
    }

    Context::~Context() = default;

    void* Context::handle() noexcept
    {
        return state_.get();
    }

    std::int32_t Context::held() const noexcept
    {
        const std::lock_guard lock(state_->mutex);
        return state_->held;
    }

    std::int64_t Context::heldBytes() const noexcept
    {
        const std::lock_guard lock(state_->mutex);
        return state_->heldBytes;
    }
}   // namespace fminus::files

namespace
{
    using fminus::files::Context;
    using fminus::files::Handle;
    using fminus::files::Kind;

    Context::State* stateOf(void* context) noexcept
    {
        return static_cast<Context::State*>(context);
    }

    // The executable's one context (fminus_rt_files_main_start), whose handle its program's global holds.
    Context* executableContext = nullptr;
}   // namespace

extern "C"
{
    void* fminus_rt_file_read(void* context, const char* name, std::int32_t length, const std::int32_t* ints,
                              std::int32_t count) noexcept
    {
        using namespace fminus::files;
        if (context == nullptr)
            return failure(Kind::File, FMINUS_FILE_REFUSED);
        try
        {
            char buffer[kMostNameBytes + 1];
            std::string_view text;
            if (!prepare(name, length, ints, count, buffer, text))
                return failure(Kind::File, FMINUS_FILE_REFUSED);
            return stateOf(context)->read(text);
        }
        catch (const std::exception&)
        {
            return failure(Kind::File, FMINUS_FILE_FAILED);
        }
    }

    std::int32_t fminus_rt_file_status(const void* handle) noexcept
    {
        const Handle* h = fminus::files::anyHandle(handle);
        return h != nullptr ? h->status : FMINUS_FILE_INVALID;
    }

    std::int32_t fminus_rt_file_size(const void* file) noexcept
    {
        const Handle* h = fminus::files::as(file, Kind::File);
        return h != nullptr ? h->size : 0;
    }

    const void* fminus_rt_file_data(const void* file) noexcept
    {
        const Handle* h = fminus::files::as(file, Kind::File);
        return h != nullptr ? h->data : nullptr;
    }

    void* fminus_rt_file_list(void* context, const char* pattern, std::int32_t length, const std::int32_t* ints,
                              std::int32_t count) noexcept
    {
        using namespace fminus::files;
        if (context == nullptr)
            return failure(Kind::List, FMINUS_FILE_REFUSED);
        try
        {
            char buffer[kMostNameBytes + 1];
            std::string_view text;
            if (!prepare(pattern, length, ints, count, buffer, text))
                return failure(Kind::List, FMINUS_FILE_REFUSED);
            return stateOf(context)->list(text);
        }
        catch (const std::exception&)
        {
            return failure(Kind::List, FMINUS_FILE_FAILED);
        }
    }

    std::int32_t fminus_rt_file_count(const void* list) noexcept
    {
        const Handle* h = fminus::files::as(list, Kind::List);
        return h != nullptr ? h->size : 0;
    }

    void* fminus_rt_file_read_nth(void* list, std::int32_t index) noexcept
    {
        using namespace fminus::files;
        const Handle* h = as(list, Kind::List);
        if (h == nullptr || h->owner == nullptr || h->list == nullptr || index < 0 || index >= h->size)
            return failure(Kind::File, FMINUS_FILE_INVALID);
        try
        {
            const std::string name = h->list->directory + h->list->names[static_cast<std::size_t>(index)];
            return h->owner->read(name);
        }
        catch (const std::exception&)
        {
            return failure(Kind::File, FMINUS_FILE_FAILED);
        }
    }

    void fminus_rt_file_close(void* handle) noexcept
    {
        auto* h = static_cast<Handle*>(handle);
        if (fminus::files::anyHandle(handle) == nullptr || h->owner == nullptr)
            return;
        try
        {
            h->owner->close(h);
        }
        catch (const std::exception&)
        {
            // std::mutex's lock can throw; the handle then stays the context's, freed with it.
        }
    }

#if FMINUS_HAS_FILES_WRITE
    std::int32_t fminus_rt_file_write(void* context, const char* name, std::int32_t length, const std::int32_t* ints,
                                      std::int32_t count, const void* data, std::int32_t bytes) noexcept
    {
        using namespace fminus::files;
        if (context == nullptr)
            return FMINUS_FILE_REFUSED;
        try
        {
            char buffer[kMostNameBytes + 1];
            std::string_view text;
            if (!prepare(name, length, ints, count, buffer, text))
                return FMINUS_FILE_REFUSED;
            return stateOf(context)->write(text, data, bytes, false);
        }
        catch (const std::exception&)
        {
            return FMINUS_FILE_FAILED;
        }
    }

    std::int32_t fminus_rt_file_append(void* context, const char* name, std::int32_t length, const std::int32_t* ints,
                                       std::int32_t count, const void* data, std::int32_t bytes) noexcept
    {
        using namespace fminus::files;
        if (context == nullptr)
            return FMINUS_FILE_REFUSED;
        try
        {
            char buffer[kMostNameBytes + 1];
            std::string_view text;
            if (!prepare(name, length, ints, count, buffer, text))
                return FMINUS_FILE_REFUSED;
            return stateOf(context)->write(text, data, bytes, true);
        }
        catch (const std::exception&)
        {
            return FMINUS_FILE_FAILED;
        }
    }
#endif
}

namespace
{
    // The entries of the environment's variable `name` (none when it is unset): separated by newlines and by the
    // system's separator of a list of paths (':' on Linux and macOS, ';' on Windows), as PATH is, so a directory may
    // hold spaces and commas. Empty entries are skipped.
    std::vector<std::string> paths(const char* name)
    {
#if defined(_WIN32)
        constexpr char separator = ';';
#else
        constexpr char separator = ':';
#endif
        std::vector<std::string> out;
        const char* text = std::getenv(name);
        if (text == nullptr)
            return out;
        std::string entry;
        for (const char* c = text;; ++c)
        {
            if (*c == '\0' || *c == '\n' || *c == separator)
            {
                if (!entry.empty())
                    out.push_back(entry);
                entry.clear();
                if (*c == '\0')
                    break;
            }
            else
                entry += *c;
        }
        return out;
    }

    // The words of the variable `name`, separated by spaces, commas, semicolons or newlines.
    std::vector<std::string> words(const char* name)
    {
        std::vector<std::string> out;
        const char* text = std::getenv(name);
        if (text == nullptr)
            return out;
        std::string word;
        for (const char* c = text;; ++c)
        {
            if (*c == '\0' || *c == ' ' || *c == ',' || *c == ';' || *c == '\n' || *c == '\t' || *c == '\r')
            {
                if (!word.empty())
                    out.push_back(word);
                word.clear();
                if (*c == '\0')
                    break;
            }
            else
                word += *c;
        }
        return out;
    }

    // Reads `variable`'s "name=directory" entries into `policy`'s folders (`write` set for each): an empty string, or
    // what is wrong.
    std::string folders(const char* variable, bool write, fminus::files::Policy& policy)
    {
        for (const std::string& entry : paths(variable))
        {
            const std::size_t equals = entry.find('=');
            if (equals == std::string::npos || equals == 0 || equals + 1 == entry.size())
                return "'" + entry + "' is not name=directory";
            fminus::files::Policy::Folder folder;
            folder.name = entry.substr(0, equals);
            folder.directory = entry.substr(equals + 1);
            folder.write = write;
            policy.folders.push_back(folder);
        }
        return {};
    }

    // The variable a policy's mistake is about, as checkPolicy and Context::create name its field.
    const char* variableOf(const std::string& error, const fminus::files::Policy& policy)
    {
        if (error.starts_with("deny"))
            return "FMINUS_FILES_DENY";
        if (error.starts_with("types"))
            return "FMINUS_FILES_TYPES";
        if (error.starts_with("anywhere"))
            return "FMINUS_FILES_ANYWHERE";
        if (error.starts_with("folders: "))
            for (const fminus::files::Policy::Folder& folder : policy.folders)
                if (error.starts_with("folders: '" + folder.name + "'"))
                    return folder.write ? "FMINUS_FILES_WRITE" : "FMINUS_FILES_READ";
        return "FMINUS_FILES_READ";
    }
}   // namespace

extern "C"
{
    std::int32_t fminus_rt_files_main_start(void** context, const char* program) noexcept
    {
        *context = nullptr;
        const char* name = program != nullptr && *program != '\0' ? program : "fminus";
        try
        {
            fminus::files::Policy policy;
            if (std::string wrong = folders("FMINUS_FILES_READ", false, policy); !wrong.empty())
            {
                std::fprintf(stderr, "%s: FMINUS_FILES_READ: %s\n", name, wrong.c_str());
                return 2;
            }
            if (std::string wrong = folders("FMINUS_FILES_WRITE", true, policy); !wrong.empty())
            {
                std::fprintf(stderr, "%s: FMINUS_FILES_WRITE: %s\n", name, wrong.c_str());
                return 2;
            }
            if (const char* anywhere = std::getenv("FMINUS_FILES_ANYWHERE"); anywhere != nullptr)
            {
                const std::string_view value(anywhere);
                if (value != "1" && value != "0" && !value.empty())
                {
                    std::fprintf(stderr, "%s: FMINUS_FILES_ANYWHERE: '%s' is not 1 or 0\n", name, anywhere);
                    return 2;
                }
                policy.anywhere = value == "1";
            }
            if (policy.folders.empty() && !policy.anywhere)
                return 0;   // the default: no files
            policy.deny = paths("FMINUS_FILES_DENY");
            policy.types = words("FMINUS_FILES_TYPES");
            std::string error;
            std::unique_ptr<Context> made = Context::create(policy, error);
            if (made == nullptr)
            {
                std::fprintf(stderr, "%s: %s: %s\n", name, variableOf(error, policy), error.c_str());
                return 2;
            }
            executableContext = made.release();
            *context = executableContext->handle();
            return 0;
        }
        catch (const std::exception& failure)
        {
            std::fprintf(stderr, "%s: FMINUS_FILES_READ: %s\n", name, failure.what());
            return 2;
        }
    }

    void fminus_rt_files_main_stop(void* context) noexcept
    {
        if (context == nullptr || executableContext == nullptr || executableContext->handle() != context)
            return;
        delete executableContext;
        executableContext = nullptr;
    }
}
#endif
