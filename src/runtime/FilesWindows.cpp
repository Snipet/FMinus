// src/runtime/FilesWindows.cpp: the file interface's system calls on Windows (FilesSystem.h), compiled into a build
// with FMINUS_FILES on Windows only. NEVER BUILT OR RUN: written where no Windows compiler was at hand
// (docs/research/files/README.md, section 5's table, is the design it follows).
//
// Everything a program names is opened with NtCreateFile relative to a directory already open (its RootDirectory),
// one segment at a time, with OBJ_DONT_REPARSE (no reparse point, a link, a junction or a mount point, may be crossed;
// a build of Windows that does not know the flag refuses it, and the open is then made without it) and
// FILE_OPEN_REPARSE_POINT (the segment itself is opened as it is, never followed), and what was opened is examined
// before it is trusted: a reparse point is refused, a file must be on a disk (GetFileType) and not a directory, and
// the name it was found by must be its own long name (GetFinalPathNameByHandleW), never an 8.3 short name such as
// "SSH~1" for ".ssh", which would slip past the rules on hidden files and types. A name never holds a separator here
// either: Files.cpp has refused "\" and "/" in a segment. Identities are the volume's serial number and the file's
// 128-bit id (FILE_ID_INFO).
//
// What creates, writes or renames a file is compiled only into a build with FMINUS_FILES_WRITE.
#include "runtime/FilesSystem.h"

#if FMINUS_HAS_FILES && defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>
#include <windows.h>
#include <winternl.h>

#ifndef FILE_OPEN
#define FILE_OPEN 0x00000001
#endif
#ifndef FILE_CREATE
#define FILE_CREATE 0x00000002
#endif
#ifndef FILE_DIRECTORY_FILE
#define FILE_DIRECTORY_FILE 0x00000001
#endif
#ifndef FILE_NON_DIRECTORY_FILE
#define FILE_NON_DIRECTORY_FILE 0x00000040
#endif
#ifndef FILE_SYNCHRONOUS_IO_NONALERT
#define FILE_SYNCHRONOUS_IO_NONALERT 0x00000020
#endif
#ifndef FILE_OPEN_REPARSE_POINT
#define FILE_OPEN_REPARSE_POINT 0x00200000
#endif
#ifndef OBJ_CASE_INSENSITIVE
#define OBJ_CASE_INSENSITIVE 0x00000040L
#endif
#ifndef OBJ_DONT_REPARSE
#define OBJ_DONT_REPARSE 0x00001000L
#endif

namespace fminus::files::os
{
    namespace
    {
        using NtCreateFileFn = LONG(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK, PLARGE_INTEGER,
                                            ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
        using NtSetInformationFileFn = LONG(NTAPI*)(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, ULONG);

        HMODULE ntdll() noexcept
        {
            return ::GetModuleHandleW(L"ntdll.dll");
        }

        NtCreateFileFn ntCreateFile() noexcept
        {
            static const auto fn =
                reinterpret_cast<NtCreateFileFn>(reinterpret_cast<void*>(::GetProcAddress(ntdll(), "NtCreateFile")));
            return fn;
        }

        // Whether OBJ_DONT_REPARSE is known to this Windows (cleared after the first STATUS_INVALID_PARAMETER).
        std::atomic<bool> dontReparse{true};

        constexpr LONG kNotFound = static_cast<LONG>(0xC0000034L);           // STATUS_OBJECT_NAME_NOT_FOUND
        constexpr LONG kPathNotFound = static_cast<LONG>(0xC000003AL);       // STATUS_OBJECT_PATH_NOT_FOUND
        constexpr LONG kNoSuchFile = static_cast<LONG>(0xC000000FL);         // STATUS_NO_SUCH_FILE
        constexpr LONG kDeletePending = static_cast<LONG>(0xC0000056L);      // STATUS_DELETE_PENDING
        constexpr LONG kAccessDenied = static_cast<LONG>(0xC0000022L);       // STATUS_ACCESS_DENIED
        constexpr LONG kSharing = static_cast<LONG>(0xC0000043L);            // STATUS_SHARING_VIOLATION
        constexpr LONG kCollision = static_cast<LONG>(0xC0000035L);          // STATUS_OBJECT_NAME_COLLISION
        constexpr LONG kInvalidParameter = static_cast<LONG>(0xC000000DL);   // STATUS_INVALID_PARAMETER
        constexpr LONG kReparse = static_cast<LONG>(0xC000050BL);            // STATUS_REPARSE_POINT_ENCOUNTERED
        constexpr LONG kNotADirectory = static_cast<LONG>(0xC0000103L);      // STATUS_NOT_A_DIRECTORY
        constexpr LONG kIsADirectory = static_cast<LONG>(0xC00000BAL);       // STATUS_FILE_IS_A_DIRECTORY
        constexpr LONG kNameInvalid = static_cast<LONG>(0xC0000033L);        // STATUS_OBJECT_NAME_INVALID

        std::int32_t statusOfNt(LONG status) noexcept
        {
            switch (status)
            {
            case kNotFound:
            case kPathNotFound:
            case kNoSuchFile:
            case kDeletePending:
                return FMINUS_FILE_MISSING;
            case kAccessDenied:
            case kSharing:
                return FMINUS_FILE_DENIED;
            case kReparse:
            case kNotADirectory:
            case kIsADirectory:
            case kNameInvalid:
            case kCollision:
                return FMINUS_FILE_REFUSED;
            default:
                return FMINUS_FILE_FAILED;
            }
        }

        std::int32_t statusOfWin32(DWORD error) noexcept
        {
            switch (error)
            {
            case ERROR_FILE_NOT_FOUND:
            case ERROR_PATH_NOT_FOUND:
                return FMINUS_FILE_MISSING;
            case ERROR_ACCESS_DENIED:
            case ERROR_SHARING_VIOLATION:
                return FMINUS_FILE_DENIED;
            case ERROR_CANT_ACCESS_FILE:
            case ERROR_CANT_RESOLVE_FILENAME:
            case ERROR_INVALID_NAME:
            case ERROR_DIRECTORY:
                return FMINUS_FILE_REFUSED;
            default:
                return FMINUS_FILE_FAILED;
            }
        }

        bool plain(std::string_view segment) noexcept
        {
            return !segment.empty() && segment != "." && segment != ".." &&
                   segment.find_first_of(std::string_view("/\\\0", 3)) == std::string_view::npos;
        }

        std::wstring wide(std::string_view text, bool& ok)
        {
            ok = false;
            if (text.empty())
            {
                ok = true;
                return {};
            }
            const int n = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                                static_cast<int>(text.size()), nullptr, 0);
            if (n <= 0)
                return {};
            std::wstring out(static_cast<std::size_t>(n), L'\0');
            ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), out.data(),
                                  n);
            ok = true;
            return out;
        }

        std::string narrow(const wchar_t* text, std::size_t length, bool& ok)
        {
            ok = false;
            if (length == 0)
            {
                ok = true;
                return {};
            }
            const int n = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, static_cast<int>(length), nullptr,
                                                0, nullptr, nullptr);
            if (n <= 0)
                return {};
            std::string out(static_cast<std::size_t>(n), '\0');
            ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, static_cast<int>(length), out.data(), n, nullptr,
                                  nullptr);
            ok = true;
            return out;
        }

        HANDLE handleOf(Native native) noexcept
        {
            return reinterpret_cast<HANDLE>(native);
        }

        Native nativeOf(HANDLE handle) noexcept
        {
            return handle == INVALID_HANDLE_VALUE || handle == nullptr ? kNone : reinterpret_cast<Native>(handle);
        }

        // Whether `handle`, opened by the name `name`, has that name as its own long name, letter case aside (NTFS
        // and FAT also find an entry by its 8.3 short name, which a program could write to reach a hidden file, or
        // one of a type not allowed, by a name the rules pass). A name that cannot be read back is refused.
        bool ownName(HANDLE handle, const std::wstring& name) noexcept
        {
            wchar_t small[512];
            std::wstring large;
            wchar_t* path = small;
            DWORD n = ::GetFinalPathNameByHandleW(handle, small, 512, FILE_NAME_NORMALIZED | VOLUME_NAME_NONE);
            if (n >= 512)
            {
                try
                {
                    large.assign(n, L'\0');
                }
                catch (...)
                {
                    return false;
                }
                n = ::GetFinalPathNameByHandleW(handle, large.data(), n, FILE_NAME_NORMALIZED | VOLUME_NAME_NONE);
                if (n >= large.size())
                    return false;
                path = large.data();
            }
            if (n == 0)
                return false;
            DWORD last = n;
            while (last > 0 && path[last - 1] != L'\\')
                --last;
            return ::CompareStringOrdinal(path + last, static_cast<int>(n - last), name.data(),
                                          static_cast<int>(name.size()), TRUE) == CSTR_EQUAL;
        }

        // Opens `segment` (UTF-8, one segment, or empty: `parent` itself again) beneath `parent`, as it is.
        std::int32_t openBeneath(Native parent, std::string_view segment, ACCESS_MASK access, ULONG disposition,
                                 ULONG options, HANDLE& out) noexcept
        {
            out = nullptr;
            if (!segment.empty() && !plain(segment))
                return FMINUS_FILE_REFUSED;
            const NtCreateFileFn create = ntCreateFile();
            if (create == nullptr)
                return FMINUS_FILE_FAILED;
            bool ok = false;
            std::wstring name;
            try
            {
                name = wide(segment, ok);
            }
            catch (...)
            {
                return FMINUS_FILE_FAILED;
            }
            if (!ok || name.size() > 32767)
                return FMINUS_FILE_REFUSED;
            UNICODE_STRING string;
            string.Buffer = name.empty() ? nullptr : name.data();
            string.Length = static_cast<USHORT>(name.size() * sizeof(wchar_t));
            string.MaximumLength = string.Length;
            for (int attempt = 0; attempt < 2; ++attempt)
            {
                const bool dont = dontReparse.load();
                OBJECT_ATTRIBUTES attributes;
                attributes.Length = sizeof attributes;
                attributes.RootDirectory = handleOf(parent);
                attributes.ObjectName = &string;
                attributes.Attributes = OBJ_CASE_INSENSITIVE | (dont ? OBJ_DONT_REPARSE : 0);
                attributes.SecurityDescriptor = nullptr;
                attributes.SecurityQualityOfService = nullptr;
                IO_STATUS_BLOCK io;
                HANDLE handle = nullptr;
                const LONG status =
                    create(&handle, access | SYNCHRONIZE, &attributes, &io, nullptr, FILE_ATTRIBUTE_NORMAL,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, disposition,
                           options | FILE_OPEN_REPARSE_POINT | FILE_SYNCHRONOUS_IO_NONALERT, nullptr, 0);
                if (status == kInvalidParameter && dont)
                {
                    dontReparse.store(false);   // a Windows before the flag: FILE_OPEN_REPARSE_POINT and the check
                    continue;                   // below still refuse every reparse point, one segment at a time
                }
                if (status < 0)
                    return statusOfNt(status);
                if (!name.empty() && !ownName(handle, name))
                {
                    ::CloseHandle(handle);
                    return FMINUS_FILE_REFUSED;
                }
                out = handle;
                return FMINUS_FILE_OK;
            }
            return FMINUS_FILE_FAILED;
        }

        struct Owned
        {
            HANDLE h = nullptr;
            explicit Owned(HANDLE handle) noexcept
                : h(handle)
            {
            }
            ~Owned()
            {
                if (h != nullptr && h != INVALID_HANDLE_VALUE)
                    ::CloseHandle(h);
            }
            Owned(const Owned&) = delete;
            Owned& operator=(const Owned&) = delete;
        };

        struct Facts
        {
            Identity identity;
            DWORD attributes = 0;
            DWORD links = 0;
            std::int64_t size = 0;
            bool disk = false;
        };

        std::int32_t factsOf(HANDLE handle, Facts& out) noexcept
        {
            BY_HANDLE_FILE_INFORMATION info;
            if (!::GetFileInformationByHandle(handle, &info))
                return statusOfWin32(::GetLastError());
            out.attributes = info.dwFileAttributes;
            out.links = info.nNumberOfLinks;
            out.size = (static_cast<std::int64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
            FILE_ID_INFO id;
            if (::GetFileInformationByHandleEx(handle, FileIdInfo, &id, sizeof id))
            {
                out.identity.volume = id.VolumeSerialNumber;
                std::memcpy(&out.identity.high, id.FileId.Identifier, 8);
                std::memcpy(&out.identity.low, id.FileId.Identifier + 8, 8);
            }
            else
            {
                out.identity.volume = info.dwVolumeSerialNumber;
                out.identity.high = 0;
                out.identity.low = (static_cast<std::uint64_t>(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
            }
            out.disk = ::GetFileType(handle) == FILE_TYPE_DISK;
            return FMINUS_FILE_OK;
        }

        bool deniedOne(const std::vector<Identity>* denied, const Identity& id) noexcept
        {
            return denied != nullptr && std::find(denied->begin(), denied->end(), id) != denied->end();
        }

        bool regular(const Facts& facts) noexcept
        {
            constexpr DWORD notRegular =
                FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DEVICE;
            return facts.disk && (facts.attributes & notRegular) == 0;
        }

        // A host's path, opened as Win32 opens one (reparse points followed: the host's own text).
        HANDLE openPath(const std::string& path, DWORD access) noexcept
        {
            bool ok = false;
            std::wstring name;
            try
            {
                name = wide(path, ok);
            }
            catch (...)
            {
                return INVALID_HANDLE_VALUE;
            }
            if (!ok || name.empty())
            {
                ::SetLastError(ERROR_INVALID_NAME);
                return INVALID_HANDLE_VALUE;
            }
            return ::CreateFileW(name.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                 OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        }

        // The final path of a handle, "\\?\C:\a\b" (or "\\?\UNC\server\share\a").
        bool finalPath(HANDLE handle, std::wstring& out)
        {
            DWORD n = ::GetFinalPathNameByHandleW(handle, nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
            if (n == 0)
                return false;
            out.assign(n, L'\0');
            n = ::GetFinalPathNameByHandleW(handle, out.data(), n, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
            if (n == 0 || n >= out.size())
                return false;
            out.resize(n);
            return true;
        }
    }   // namespace

    Dir::~Dir()
    {
        if (native_ != kNone)
            ::CloseHandle(handleOf(native_));
    }

    Dir& Dir::operator=(Dir&& other) noexcept
    {
        if (this != &other)
        {
            if (native_ != kNone)
                ::CloseHandle(handleOf(native_));
            native_ = other.native_;
            other.native_ = kNone;
        }
        return *this;
    }

    std::int32_t openFolder(const std::string& path, Dir& out, std::string& why)
    {
        const HANDLE handle = openPath(path, FILE_LIST_DIRECTORY | FILE_TRAVERSE | FILE_READ_ATTRIBUTES | SYNCHRONIZE);
        if (handle == INVALID_HANDLE_VALUE)
        {
            const DWORD error = ::GetLastError();
            why = "error " + std::to_string(error);
            return statusOfWin32(error);
        }
        out = Dir(nativeOf(handle));
        Facts facts;
        if (factsOf(handle, facts) != FMINUS_FILE_OK || (facts.attributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
        {
            why = "it is not a directory";
            out = Dir();
            return FMINUS_FILE_REFUSED;
        }
        return FMINUS_FILE_OK;
    }

    std::int32_t openDenied(const std::string& path, Dir& out)
    {
        const HANDLE handle = openPath(path, FILE_READ_ATTRIBUTES);
        if (handle == INVALID_HANDLE_VALUE)
            return statusOfWin32(::GetLastError());
        out = Dir(nativeOf(handle));
        return FMINUS_FILE_OK;
    }

    std::int32_t identityOf(Native native, Identity& out)
    {
        Facts facts;
        if (const std::int32_t status = factsOf(handleOf(native), facts); status != FMINUS_FILE_OK)
            return status;
        out = facts.identity;
        return FMINUS_FILE_OK;
    }

    std::int32_t lookUp(const std::string& path, Identity& out)
    {
        Dir dir;
        if (const std::int32_t status = openDenied(path, dir); status != FMINUS_FILE_OK)
            return status;
        return identityOf(dir.get(), out);
    }

    std::int32_t ancestors(Native directory, std::vector<Identity>& out)
    {
        std::wstring path;
        if (!finalPath(handleOf(directory), path))
            return statusOfWin32(::GetLastError());
        // The root: "\\?\C:\" or "\\?\UNC\server\share\"; each directory after it a prefix, the directory's own last.
        std::size_t root = 0;
        if (path.rfind(L"\\\\?\\UNC\\", 0) == 0)
        {
            const std::size_t server = path.find(L'\\', 8);
            const std::size_t share = server == std::wstring::npos ? server : path.find(L'\\', server + 1);
            root = share == std::wstring::npos ? path.size() : share + 1;
        }
        else if (path.rfind(L"\\\\?\\", 0) == 0 && path.size() >= 7)
            root = 7;   // "\\?\C:\"
        else
            return FMINUS_FILE_REFUSED;
        std::vector<std::size_t> ends{root};
        for (std::size_t i = root; i < path.size(); ++i)
            if (path[i] == L'\\')
                ends.push_back(i);
        for (const std::size_t end : ends)
        {
            const std::wstring prefix = path.substr(0, end);
            const HANDLE handle = ::CreateFileW(prefix.c_str(), FILE_READ_ATTRIBUTES,
                                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                                OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
            if (handle == INVALID_HANDLE_VALUE)
                return statusOfWin32(::GetLastError());
            const Owned owned(handle);
            Facts facts;
            if (const std::int32_t status = factsOf(handle, facts); status != FMINUS_FILE_OK)
                return status;
            out.push_back(facts.identity);
        }
        return FMINUS_FILE_OK;
    }

    std::int32_t openDirectory(Native parent, std::string_view segment, Dir& out)
    {
        if (!plain(segment))
            return FMINUS_FILE_REFUSED;
        HANDLE handle = nullptr;
        if (const std::int32_t status =
                openBeneath(parent, segment, FILE_LIST_DIRECTORY | FILE_TRAVERSE | FILE_READ_ATTRIBUTES, FILE_OPEN,
                            FILE_DIRECTORY_FILE, handle);
            status != FMINUS_FILE_OK)
            return status;
        Dir dir(nativeOf(handle));
        Facts facts;
        if (const std::int32_t status = factsOf(handle, facts); status != FMINUS_FILE_OK)
            return status;
        if ((facts.attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
            (facts.attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
            return FMINUS_FILE_REFUSED;
        out = std::move(dir);
        return FMINUS_FILE_OK;
    }

#if FMINUS_HAS_FILES_ANYWHERE
    std::int32_t realDirectory(const std::string& path, std::string& real)
    {
        std::string native = path;
        std::replace(native.begin(), native.end(), '/', '\\');
        const HANDLE handle = openPath(native, FILE_READ_ATTRIBUTES);
        if (handle == INVALID_HANDLE_VALUE)
            return statusOfWin32(::GetLastError());
        const Owned owned(handle);
        std::wstring final;
        if (!finalPath(handle, final))
            return statusOfWin32(::GetLastError());
        // Only a drive's path: a UNC path ("\\?\UNC\...") can be written to slip past a list (Chromium refuses them).
        if (final.rfind(L"\\\\?\\", 0) != 0 || final.size() < 7 || final[5] != L':' || final[6] != L'\\')
            return FMINUS_FILE_REFUSED;
        bool ok = false;
        real = narrow(final.data() + 4, final.size() - 4, ok);
        if (!ok)
            return FMINUS_FILE_REFUSED;
        std::replace(real.begin(), real.end(), '\\', '/');
        while (real.size() > 3 && real.back() == '/')
            real.pop_back();
        return FMINUS_FILE_OK;
    }

    std::int32_t openRoot(const std::string& real, Dir& out, std::size_t& rest)
    {
        if (absolutePrefix(real) != 3)
            return FMINUS_FILE_REFUSED;
        const std::string root = std::string("\\\\?\\") + real.substr(0, 2) + "\\";
        const HANDLE handle = openPath(root, FILE_LIST_DIRECTORY | FILE_TRAVERSE | FILE_READ_ATTRIBUTES | SYNCHRONIZE);
        if (handle == INVALID_HANDLE_VALUE)
            return statusOfWin32(::GetLastError());
        out = Dir(nativeOf(handle));
        rest = 3;
        return FMINUS_FILE_OK;
    }
#endif

    std::size_t absolutePrefix(std::string_view name) noexcept
    {
        if (name.size() < 3 || name[1] != ':' || name[2] != '/')
            return 0;
        const char drive = name[0];
        return (drive >= 'A' && drive <= 'Z') || (drive >= 'a' && drive <= 'z') ? 3 : 0;
    }

    std::int32_t readFile(Native directory, std::string_view segment, const ReadRules& rules, unsigned char*& data,
                          std::int32_t& size)
    {
        data = nullptr;
        size = 0;
        if (!plain(segment) || rules.limit < 0)
            return FMINUS_FILE_REFUSED;
        HANDLE handle = nullptr;
        if (const std::int32_t status = openBeneath(directory, segment, FILE_READ_DATA | FILE_READ_ATTRIBUTES,
                                                    FILE_OPEN, FILE_NON_DIRECTORY_FILE | FILE_SEQUENTIAL_ONLY, handle);
            status != FMINUS_FILE_OK)
            return status;
        const Owned file(handle);
        Facts facts;
        if (const std::int32_t status = factsOf(handle, facts); status != FMINUS_FILE_OK)
            return status;
        if (!regular(facts) || deniedOne(rules.denied, facts.identity))
            return FMINUS_FILE_REFUSED;
        if (rules.singleLink && facts.links > 1)
            return FMINUS_FILE_REFUSED;
        if (facts.size > rules.limit)
            return FMINUS_FILE_LIMIT;
        const std::size_t most = static_cast<std::size_t>(rules.limit) + 1;
        std::size_t capacity = std::min<std::size_t>(static_cast<std::size_t>(facts.size) + 1, most);
        auto* buffer = static_cast<unsigned char*>(std::malloc(capacity + 1));
        if (buffer == nullptr)
            return FMINUS_FILE_FAILED;
        std::size_t got = 0;
        for (;;)
        {
            if (got == capacity)
            {
                if (capacity == most)
                    break;
                const std::size_t larger =
                    std::min(most, capacity < (std::size_t{1} << 20) ? std::size_t{1} << 20 : capacity * 2);
                auto* grown = static_cast<unsigned char*>(std::realloc(buffer, larger + 1));
                if (grown == nullptr)
                {
                    std::free(buffer);
                    return FMINUS_FILE_FAILED;
                }
                buffer = grown;
                capacity = larger;
            }
            DWORD n = 0;
            const DWORD ask = static_cast<DWORD>(std::min<std::size_t>(capacity - got, 1u << 30));
            if (!::ReadFile(handle, buffer + got, ask, &n, nullptr))
            {
                std::free(buffer);
                return FMINUS_FILE_FAILED;
            }
            if (n == 0)
                break;
            got += n;
        }
        if (got > static_cast<std::size_t>(rules.limit))
        {
            std::free(buffer);
            return FMINUS_FILE_LIMIT;
        }
        buffer[got] = 0;
        data = buffer;
        size = static_cast<std::int32_t>(got);
        return FMINUS_FILE_OK;
    }

    void checkEveryName(bool) noexcept
    {
        // Every name is checked on Windows already (ownName, in openBeneath).
    }

    std::int32_t nameListed(Native directory, std::string_view segment)
    {
        bool ok = false;
        std::wstring want;
        try
        {
            want = wide(segment, ok);
        }
        catch (...)
        {
            return FMINUS_FILE_FAILED;
        }
        if (!ok || want.empty())
            return FMINUS_FILE_REFUSED;
        HANDLE handle = nullptr;
        if (const std::int32_t status =
                openBeneath(directory, {}, FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES, FILE_OPEN, FILE_DIRECTORY_FILE,
                            handle);
            status != FMINUS_FILE_OK)
            return status;
        const Owned owned(handle);
        alignas(8) unsigned char buffer[64 * 1024];
        FILE_INFO_BY_HANDLE_CLASS kind = FileIdBothDirectoryRestartInfo;
        for (;;)
        {
            if (!::GetFileInformationByHandleEx(handle, kind, buffer, sizeof buffer))
            {
                const DWORD error = ::GetLastError();
                return error == ERROR_NO_MORE_FILES ? FMINUS_FILE_REFUSED : statusOfWin32(error);
            }
            kind = FileIdBothDirectoryInfo;
            for (std::size_t at = 0;;)
            {
                const auto* item = reinterpret_cast<const FILE_ID_BOTH_DIR_INFO*>(buffer + at);
                if (::CompareStringOrdinal(item->FileName, static_cast<int>(item->FileNameLength / sizeof(wchar_t)),
                                           want.data(), static_cast<int>(want.size()), TRUE) == CSTR_EQUAL)
                    return FMINUS_FILE_OK;
                if (item->NextEntryOffset == 0)
                    break;
                at += item->NextEntryOffset;
            }
        }
    }

    std::int32_t listDirectory(Native directory, Lister& lister)
    {
        // A handle of its own: a directory's enumeration is the handle's, and the folder's is shared by every thread.
        HANDLE handle = nullptr;
        if (const std::int32_t status = openBeneath(directory, {}, FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
                                                    FILE_OPEN, FILE_DIRECTORY_FILE, handle);
            status != FMINUS_FILE_OK)
            return status;
        const Owned owned(handle);
        alignas(8) unsigned char buffer[64 * 1024];
        FILE_INFO_BY_HANDLE_CLASS kind = FileIdBothDirectoryRestartInfo;
        for (;;)
        {
            if (!::GetFileInformationByHandleEx(handle, kind, buffer, sizeof buffer))
            {
                const DWORD error = ::GetLastError();
                return error == ERROR_NO_MORE_FILES ? FMINUS_FILE_OK : statusOfWin32(error);
            }
            kind = FileIdBothDirectoryInfo;
            std::size_t at = 0;
            for (;;)
            {
                const auto* item = reinterpret_cast<const FILE_ID_BOTH_DIR_INFO*>(buffer + at);
                bool ok = false;
                const std::string name = narrow(item->FileName, item->FileNameLength / sizeof(wchar_t), ok);
                if (ok && plain(name) && lister.wanted(name))
                {
                    Entry entry;
                    entry.name = name;
                    const DWORD attributes = item->FileAttributes;
                    constexpr DWORD notRegular =
                        FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DEVICE;
                    if ((attributes & notRegular) == 0)
                    {
                        HANDLE file = nullptr;
                        if (openBeneath(nativeOf(handle), name, FILE_READ_ATTRIBUTES, FILE_OPEN,
                                        FILE_NON_DIRECTORY_FILE, file) == FMINUS_FILE_OK)
                        {
                            const Owned opened(file);
                            Facts facts;
                            if (factsOf(file, facts) == FMINUS_FILE_OK && regular(facts))
                            {
                                entry.regular = true;
                                entry.links = facts.links;
                            }
                        }
                    }
                    if (const std::int32_t status = lister.entry(entry); status != FMINUS_FILE_OK)
                        return status;
                }
                if (item->NextEntryOffset == 0)
                    break;
                at += item->NextEntryOffset;
            }
        }
    }

#if FMINUS_HAS_FILES_WRITE
    namespace
    {
        std::atomic<int> failNext{0};

        using NtSetInformationFileFnT = NtSetInformationFileFn;

        NtSetInformationFileFnT ntSetInformationFile() noexcept
        {
            static const auto fn = reinterpret_cast<NtSetInformationFileFnT>(
                reinterpret_cast<void*>(::GetProcAddress(ntdll(), "NtSetInformationFile")));
            return fn;
        }

        bool writeAll(HANDLE handle, const void* data, std::size_t bytes) noexcept
        {
            const auto* at = static_cast<const unsigned char*>(data);
            while (bytes > 0)
            {
                DWORD n = 0;
                const DWORD ask = static_cast<DWORD>(std::min<std::size_t>(bytes, 1u << 30));
                if (!::WriteFile(handle, at, ask, &n, nullptr) || n == 0)
                    return false;
                at += n;
                bytes -= n;
            }
            return true;
        }

        std::string temporaryName()
        {
            static std::atomic<std::uint64_t> counter{0};
            LARGE_INTEGER now;
            ::QueryPerformanceCounter(&now);
            std::uint64_t x = static_cast<std::uint64_t>(now.QuadPart) ^
                              (static_cast<std::uint64_t>(::GetCurrentProcessId()) << 32) ^
                              (counter.fetch_add(1) * 0x9e3779b97f4a7c15ULL);
            x ^= x >> 31;
            x *= 0xbf58476d1ce4e5b9ULL;
            x ^= x >> 29;
            static const char digits[] = "0123456789abcdef";
            std::string name = ".fminus-";
            for (int i = 15; i >= 0; --i)
                name += digits[(x >> (4 * i)) & 15];
            return name + ".tmp";
        }

        // Marks an open file to be deleted when its last handle closes, or (`keep`) no longer: the temporary file is
        // marked from when it is made until just before it is renamed, so a process that dies half way leaves none.
        bool discard(HANDLE handle, bool keep = false) noexcept
        {
            FILE_DISPOSITION_INFO disposition;
            disposition.DeleteFile = keep ? FALSE : TRUE;
            return ::SetFileInformationByHandle(handle, FileDispositionInfo, &disposition, sizeof disposition) != 0;
        }

        // Whether `name` is one temporaryName makes: ".fminus-", 16 lower-case hexadecimal digits, ".tmp".
        bool temporaryShape(const std::wstring& name) noexcept
        {
            const std::wstring_view head = L".fminus-";
            const std::wstring_view tail = L".tmp";
            const std::wstring_view all(name);
            if (all.size() != head.size() + 16 + tail.size() || all.substr(0, head.size()) != head ||
                all.substr(all.size() - tail.size()) != tail)
                return false;
            for (const wchar_t c : all.substr(head.size(), 16))
                if (!((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f')))
                    return false;
            return true;
        }

        // NtSetInformationFile's FILE_RENAME_INFORMATION (class 10) and its Ex form (class 65, Windows 10 1607 on),
        // whose first field is a BOOLEAN in the one and a ULONG of flags in the other: a name relative to
        // RootDirectory.
        struct RenameEx
        {
            ULONG flags;
            HANDLE root;
            ULONG length;
            WCHAR name[1];
        };

        // Renames the open file `handle` to `name` beneath `directory`, replacing what is there (never following it), a
        // few times over: other software (a virus scanner, an indexer) holds files open on Windows for a moment.
        std::int32_t renameBeneath(HANDLE handle, Native directory, const std::wstring& name)
        {
            const NtSetInformationFileFnT set = ntSetInformationFile();
            if (set == nullptr)
                return FMINUS_FILE_FAILED;
            const std::size_t bytes = offsetof(RenameEx, name) + (name.size() + 1) * sizeof(wchar_t);
            std::vector<unsigned char> block(bytes, 0);
            auto* info = reinterpret_cast<RenameEx*>(block.data());
            info->root = handleOf(directory);
            info->length = static_cast<ULONG>(name.size() * sizeof(wchar_t));
            std::memcpy(info->name, name.data(), name.size() * sizeof(wchar_t));
            LONG status = 0;
            for (int attempt = 0; attempt < 5; ++attempt)
            {
                IO_STATUS_BLOCK io;
                info->flags = 0x1 | 0x2;   // FILE_RENAME_REPLACE_IF_EXISTS | FILE_RENAME_POSIX_SEMANTICS
                status = set(handle, &io, info, static_cast<ULONG>(bytes), 65);   // FileRenameInformationEx
                if (status == kInvalidParameter || status == static_cast<LONG>(0xC0000003L))   // _INVALID_INFO_CLASS
                {
                    info->flags = 1;   // BOOLEAN ReplaceIfExists = TRUE, in the first byte; the rest of the ULONG 0
                    status = set(handle, &io, info, static_cast<ULONG>(bytes), 10);   // FileRenameInformation
                }
                if (status >= 0)
                    return FMINUS_FILE_OK;
                if (status != kAccessDenied && status != kSharing)
                    break;
                ::Sleep(10 << attempt);
            }
            return statusOfNt(status);
        }

        // What is at the target: FILE_OK (a regular file, not denied), FILE_MISSING, or why not.
        std::int32_t target(Native directory, std::string_view name, const WriteRules& rules, Facts& facts)
        {
            HANDLE handle = nullptr;
            if (const std::int32_t status =
                    openBeneath(directory, name, FILE_READ_ATTRIBUTES, FILE_OPEN, FILE_NON_DIRECTORY_FILE, handle);
                status != FMINUS_FILE_OK)
                return status;
            const Owned owned(handle);
            if (const std::int32_t status = factsOf(handle, facts); status != FMINUS_FILE_OK)
                return status;
            if (!regular(facts) || deniedOne(rules.denied, facts.identity))
                return FMINUS_FILE_REFUSED;
            return FMINUS_FILE_OK;
        }
    }   // namespace

    void failNextReplaceAt(int step) noexcept
    {
        failNext.store(step);
    }

    std::int32_t replaceFile(Native directory, std::string_view segment, const void* data, std::int32_t bytes,
                             const WriteRules& rules, bool& created)
    {
        created = false;
        const int fail = failNext.exchange(0);
        if (!plain(segment) || bytes < 0 || (data == nullptr && bytes > 0))
            return FMINUS_FILE_REFUSED;
        if (bytes > rules.limit)
            return FMINUS_FILE_LIMIT;
        Facts facts;
        if (const std::int32_t status = target(directory, segment, rules, facts);
            status != FMINUS_FILE_OK && status != FMINUS_FILE_MISSING)
            return status;
        bool ok = false;
        const std::wstring wideName = wide(segment, ok);
        if (!ok)
            return FMINUS_FILE_REFUSED;
        HANDLE handle = nullptr;
        std::int32_t made = FMINUS_FILE_FAILED;
        for (int attempt = 0; attempt < 16 && made != FMINUS_FILE_OK; ++attempt)
        {
            if (fail == 1)
                break;
            made = openBeneath(directory, temporaryName(), GENERIC_WRITE | DELETE | FILE_READ_ATTRIBUTES, FILE_CREATE,
                               FILE_NON_DIRECTORY_FILE, handle);
            if (made != FMINUS_FILE_OK && made != FMINUS_FILE_REFUSED)   // a collision is refused: another name
                break;
        }
        if (made != FMINUS_FILE_OK)
            return fail == 1 ? FMINUS_FILE_FAILED : made;
        created = true;
        const Owned file(handle);
        if (!discard(handle))
            return FMINUS_FILE_FAILED;   // (closing the handle leaves the empty file: sweepTemporaries takes it)
        const auto failed = [&](std::int32_t status)
        {
            discard(handle);
            return status;
        };
        if (fail == 2 || !writeAll(handle, data, static_cast<std::size_t>(bytes)))
            return failed(FMINUS_FILE_FAILED);
        if (fail == 3 || !::FlushFileBuffers(handle))
            return failed(FMINUS_FILE_FAILED);
        if (fail == 4)
            return failed(FMINUS_FILE_FAILED);
        if (fail == 5)
            return failed(FMINUS_FILE_FAILED);
        if (!discard(handle, true))
            return failed(FMINUS_FILE_FAILED);
        if (const std::int32_t status = renameBeneath(handle, directory, wideName); status != FMINUS_FILE_OK)
            return failed(status);
        if (fail == 6)
            return FMINUS_FILE_FAILED;   // the new file is in place (Windows has no flush of a directory here)
        return FMINUS_FILE_OK;
    }

    void sweepTemporaries(Native directory)
    {
        HANDLE handle = nullptr;
        if (openBeneath(directory, {}, FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES, FILE_OPEN, FILE_DIRECTORY_FILE,
                        handle) != FMINUS_FILE_OK)
            return;
        const Owned owned(handle);
        FILETIME now;
        ::GetSystemTimeAsFileTime(&now);
        const std::int64_t stale = ((static_cast<std::int64_t>(now.dwHighDateTime) << 32) | now.dwLowDateTime) -
                                   kStaleTemporarySeconds * 10000000;   // FILETIME counts 100 ns
        std::vector<std::wstring> found;
        alignas(8) unsigned char buffer[64 * 1024];
        FILE_INFO_BY_HANDLE_CLASS kind = FileIdBothDirectoryRestartInfo;
        try
        {
            while (::GetFileInformationByHandleEx(handle, kind, buffer, sizeof buffer))
            {
                kind = FileIdBothDirectoryInfo;
                for (std::size_t at = 0;;)
                {
                    const auto* item = reinterpret_cast<const FILE_ID_BOTH_DIR_INFO*>(buffer + at);
                    const std::wstring name(item->FileName, item->FileNameLength / sizeof(wchar_t));
                    constexpr DWORD notRegular =
                        FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DEVICE;
                    if ((item->FileAttributes & notRegular) == 0 && item->LastWriteTime.QuadPart < stale &&
                        temporaryShape(name))
                        found.push_back(name);
                    if (item->NextEntryOffset == 0)
                        break;
                    at += item->NextEntryOffset;
                }
            }
        }
        catch (...)
        {
            return;
        }
        for (const std::wstring& name : found)
        {
            bool ok = false;
            std::string utf8;
            try
            {
                utf8 = narrow(name.data(), name.size(), ok);
            }
            catch (...)
            {
                return;
            }
            HANDLE file = nullptr;
            if (!ok || openBeneath(directory, utf8, DELETE | FILE_READ_ATTRIBUTES, FILE_OPEN, FILE_NON_DIRECTORY_FILE,
                                   file) != FMINUS_FILE_OK)
                continue;
            const Owned opened(file);
            Facts facts;
            if (factsOf(file, facts) == FMINUS_FILE_OK && regular(facts) && facts.links == 1)
                discard(file);
        }
    }

    std::int32_t appendFile(Native directory, std::string_view segment, const void* data, std::int32_t bytes,
                            const WriteRules& rules, bool& created)
    {
        created = false;
        if (!plain(segment) || bytes < 0 || (data == nullptr && bytes > 0))
            return FMINUS_FILE_REFUSED;
        if (bytes > rules.limit)
            return FMINUS_FILE_LIMIT;
        Facts before;
        const std::int32_t found = target(directory, segment, rules, before);
        if (found != FMINUS_FILE_OK && found != FMINUS_FILE_MISSING)
            return found;
        if (found == FMINUS_FILE_MISSING && !rules.mayCreate)
            return FMINUS_FILE_LIMIT;
        HANDLE handle = nullptr;
        if (const std::int32_t status =
                openBeneath(directory, segment, FILE_APPEND_DATA | FILE_READ_ATTRIBUTES,
                            found == FMINUS_FILE_OK ? FILE_OPEN : FILE_CREATE, FILE_NON_DIRECTORY_FILE, handle);
            status != FMINUS_FILE_OK)
            return status;
        created = found == FMINUS_FILE_MISSING;
        const Owned file(handle);
        Facts facts;
        if (const std::int32_t status = factsOf(handle, facts); status != FMINUS_FILE_OK)
            return status;
        if (!regular(facts) || facts.links > 1 || deniedOne(rules.denied, facts.identity))
            return FMINUS_FILE_REFUSED;
        if (!created && !(facts.identity == before.identity))
            return FMINUS_FILE_REFUSED;
        if (facts.size + bytes > rules.limit)
            return FMINUS_FILE_LIMIT;
        return writeAll(handle, data, static_cast<std::size_t>(bytes)) ? FMINUS_FILE_OK : FMINUS_FILE_FAILED;
    }
#endif
}   // namespace fminus::files::os
#endif
