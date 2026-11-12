// src/runtime/FilesPosix.cpp: the file interface's system calls on Linux and macOS (FilesSystem.h), compiled into a
// build with FMINUS_FILES, on every system but Windows. Everything a program names is opened with openat beneath a
// directory already open, one segment at a time, with O_NOFOLLOW (and on macOS O_NOFOLLOW_ANY as well), and what was
// opened is examined with fstat before it is trusted: a symbolic link is never followed, a FIFO never waited on, a
// device never opened (it is looked at with fstatat first, and only a regular file is opened at all). Linux's openat2
// could do the walk in one call; it is not used, because the walk must stop at each directory to compare it with the
// denied ones anyway, and a sandbox may refuse the call.
//
// What creates, writes or renames a file is compiled only into a build with FMINUS_FILES_WRITE.
#include "runtime/FilesSystem.h"

#if FMINUS_HAS_FILES && !defined(_WIN32)
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
#if defined(__linux__)
#include <sys/vfs.h>
#elif defined(__APPLE__)
#include <sys/mount.h>
#include <sys/param.h>
#endif

namespace fminus::files::os
{
    namespace
    {
#if defined(__linux__) && defined(O_PATH)
        // A directory walked through needs search permission only, and is never read: O_PATH opens it without asking
        // for more (openat and fstat take such a descriptor; a listing opens "." beneath it to read it).
        constexpr int kWalk = O_PATH;
#else
        constexpr int kWalk = O_RDONLY;
#endif
#if defined(O_NOFOLLOW_ANY)
        constexpr int kNoFollow = O_NOFOLLOW | O_NOFOLLOW_ANY;   // macOS 11: no link anywhere in what is given
#else
        constexpr int kNoFollow = O_NOFOLLOW;
#endif

        // What an errno means to a program.
        std::int32_t statusOf(int error) noexcept
        {
            switch (error)
            {
            case ENOENT:
                return FMINUS_FILE_MISSING;
            case ELOOP:     // a link where O_NOFOLLOW forbids one
            case EMLINK:    // the same, on the BSDs
            case ENOTDIR:   // something that is not a directory, where one is walked through
            case EISDIR:    // a directory where a file was named
            case ENXIO:     // a FIFO or a socket opened for writing, a device without its driver
            case ENODEV:
            case ENAMETOOLONG:   // never, for a segment of 255 bytes; refused rather than shortened
            case EXDEV:
                return FMINUS_FILE_REFUSED;
            case EACCES:
            case EPERM:
            case EROFS:
                return FMINUS_FILE_DENIED;
            default:
                return FMINUS_FILE_FAILED;
            }
        }

        // A segment the system may be given: never empty, never "." or "..", and never with a separator or a NUL in
        // it. Files.cpp has refused all of these already; this is the last word before the kernel's.
        bool plain(std::string_view segment) noexcept
        {
            return !segment.empty() && segment != "." && segment != ".." &&
                   segment.find_first_of(std::string_view("/\0", 2)) == std::string_view::npos;
        }

        int openAt(Native directory, const std::string& segment, int flags, mode_t mode = 0) noexcept
        {
            int fd = -1;
            do
                fd = ::openat(static_cast<int>(directory), segment.c_str(), flags | O_CLOEXEC, mode);
            while (fd < 0 && errno == EINTR);
            return fd;
        }

        Identity identity(const struct stat& st) noexcept
        {
            Identity id;
            id.volume = static_cast<std::uint64_t>(st.st_dev);
            id.low = static_cast<std::uint64_t>(st.st_ino);
            return id;
        }

        bool deniedOne(const std::vector<Identity>* denied, const Identity& id) noexcept
        {
            if (denied == nullptr)
                return false;
            for (const Identity& d : *denied)
                if (d == id)
                    return true;
            return false;
        }

        // Closes a descriptor when it goes.
        struct Fd
        {
            int fd = -1;
            explicit Fd(int f) noexcept
                : fd(f)
            {
            }
            ~Fd()
            {
                if (fd >= 0)
                    ::close(fd);
            }
            Fd(const Fd&) = delete;
            Fd& operator=(const Fd&) = delete;
        };

#if defined(__linux__)
        // Whether the descriptor's file is on one of the kernel's own file systems, whose "regular files" are not data
        // on a disk: /proc, /sys and the like (their sizes say 0, their reads run the kernel's code).
        bool kernelFileSystem(int fd) noexcept
        {
            struct statfs fs;
            if (::fstatfs(fd, &fs) != 0)
                return true;   // what cannot be told is refused
            switch (static_cast<unsigned long>(fs.f_type))
            {
            case 0x9fa0UL:       // proc
            case 0x62656572UL:   // sysfs
            case 0x64626720UL:   // debugfs
            case 0x74726163UL:   // tracefs
            case 0x73636673UL:   // securityfs
            case 0x27e0ebUL:     // cgroup
            case 0x63677270UL:   // cgroup2
            case 0xcafe4a11UL:   // bpf
            case 0x62656570UL:   // configfs
            case 0x6165676cUL:   // pstore
            case 0xde5e81e4UL:   // efivarfs
            case 0x1cd1UL:       // devpts
            case 0x42494e4dUL:   // binfmt_misc
            case 0x65735543UL:   // fusectl
            case 0xf97cff8cUL:   // selinuxfs
            case 0x43415d53UL:   // smackfs
            case 0x6e736673UL:   // nsfs
                return true;
            default:
                return false;
            }
        }
#endif

        std::int32_t opened(int fd, Dir& out) noexcept
        {
            if (fd < 0)
                return statusOf(errno);
            out = Dir(fd);
            return FMINUS_FILE_OK;
        }

        // checkEveryName's switch: every file system's names checked against its listings.
        std::atomic<bool> everyName{false};

        bool sameLetters(std::string_view a, std::string_view b) noexcept
        {
            if (a.size() != b.size())
                return false;
            for (std::size_t i = 0; i < a.size(); ++i)
            {
                const char x = a[i] >= 'A' && a[i] <= 'Z' ? static_cast<char>(a[i] - 'A' + 'a') : a[i];
                const char y = b[i] >= 'A' && b[i] <= 'Z' ? static_cast<char>(b[i] - 'A' + 'a') : b[i];
                if (x != y)
                    return false;
            }
            return true;
        }

        // Whether the file system of the directory `fd` is known to find an entry by its one name only (case
        // aside): the native file systems of Linux and macOS. Any other (FAT and NTFS with their 8.3 short names, a
        // Windows share, WSL's drives, FUSE, NFS) may answer to a second name, so its names are checked (nameListed).
        bool onePerEntry(int fd) noexcept
        {
            if (everyName.load(std::memory_order_relaxed))
                return false;
#if defined(__linux__)
            struct statfs fs;
            if (::fstatfs(fd, &fs) != 0)
                return false;
            switch (static_cast<unsigned long>(fs.f_type))
            {
            case 0xef53UL:       // ext2, ext3, ext4
            case 0x58465342UL:   // xfs
            case 0x9123683eUL:   // btrfs
            case 0x01021994UL:   // tmpfs
            case 0x858458f6UL:   // ramfs
            case 0x794c7630UL:   // overlayfs
            case 0x2fc12fc1UL:   // zfs
            case 0xf2f52010UL:   // f2fs
            case 0xca451a4eUL:   // bcachefs
            case 0x73717368UL:   // squashfs
            case 0xe0f5e1e2UL:   // erofs
                return true;
            default:
                return false;
            }
#elif defined(__APPLE__)
            struct statfs fs;
            if (::fstatfs(fd, &fs) != 0)
                return false;
            return std::strcmp(fs.f_fstypename, "apfs") == 0 || std::strcmp(fs.f_fstypename, "hfs") == 0;
#else
            return false;
#endif
        }
    }   // namespace

    Dir::~Dir()
    {
        if (native_ != kNone)
            ::close(static_cast<int>(native_));
    }

    Dir& Dir::operator=(Dir&& other) noexcept
    {
        if (this != &other)
        {
            if (native_ != kNone)
                ::close(static_cast<int>(native_));
            native_ = other.native_;
            other.native_ = kNone;
        }
        return *this;
    }

    std::int32_t openFolder(const std::string& path, Dir& out, std::string& why)
    {
        if (path.find('\0') != std::string::npos)
        {
            why = "it has a NUL byte";
            return FMINUS_FILE_REFUSED;
        }
        int fd = -1;
        do
            fd = ::open(path.c_str(), kWalk | O_DIRECTORY | O_CLOEXEC);
        while (fd < 0 && errno == EINTR);
        if (fd < 0)
        {
            why = std::strerror(errno);
            return statusOf(errno);
        }
        out = Dir(fd);
        struct stat st;
        if (::fstat(fd, &st) != 0 || !S_ISDIR(st.st_mode))
        {
            why = "it is not a directory";
            out = Dir();
            return FMINUS_FILE_REFUSED;
        }
        return FMINUS_FILE_OK;
    }

    std::int32_t openDenied(const std::string& path, Dir& out)
    {
        if (path.find('\0') != std::string::npos)
            return FMINUS_FILE_REFUSED;
        int fd = -1;
        do
            fd = ::open(path.c_str(), kWalk | O_NONBLOCK | O_NOCTTY | O_CLOEXEC);
        while (fd < 0 && errno == EINTR);
        if (fd < 0 && errno == ENOTDIR)
            return FMINUS_FILE_MISSING;   // a file on the way: nothing is at the path
        return opened(fd, out);
    }

    std::int32_t identityOf(Native native, Identity& out)
    {
        struct stat st;
        if (::fstat(static_cast<int>(native), &st) != 0)
            return statusOf(errno);
        out = identity(st);
        return FMINUS_FILE_OK;
    }

    std::int32_t lookUp(const std::string& path, Identity& out)
    {
        if (path.find('\0') != std::string::npos)
            return FMINUS_FILE_REFUSED;
        struct stat st;
        if (::stat(path.c_str(), &st) != 0)
            return errno == ENOENT || errno == ENOTDIR ? FMINUS_FILE_MISSING : statusOf(errno);
        out = identity(st);
        return FMINUS_FILE_OK;
    }

    std::int32_t ancestors(Native directory, std::vector<Identity>& out)
    {
        Identity here;
        if (const std::int32_t status = identityOf(directory, here); status != FMINUS_FILE_OK)
            return status;
        Dir current;
        Native at = directory;
        for (int depth = 0; depth < 4096; ++depth)
        {
            Dir up;
            if (const std::int32_t status = opened(openAt(at, "..", kWalk | O_DIRECTORY | O_NOFOLLOW), up);
                status != FMINUS_FILE_OK)
                return status;
            Identity parent;
            if (const std::int32_t status = identityOf(up.get(), parent); status != FMINUS_FILE_OK)
                return status;
            if (parent == here)
                return FMINUS_FILE_OK;   // the root: its ".." is itself
            out.push_back(parent);
            here = parent;
            current = std::move(up);
            at = current.get();
        }
        return FMINUS_FILE_REFUSED;   // deeper than any real tree: refused rather than half checked
    }

    void checkEveryName(bool every) noexcept
    {
        everyName.store(every);
    }

    std::int32_t nameListed(Native directory, std::string_view segment)
    {
        const int fd = openAt(directory, ".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
        if (fd < 0)
            return statusOf(errno);
        DIR* stream = ::fdopendir(fd);
        if (stream == nullptr)
        {
            const int error = errno;
            ::close(fd);
            return error == ENOMEM ? FMINUS_FILE_FAILED : statusOf(error);
        }
        std::int32_t result = FMINUS_FILE_REFUSED;
        for (;;)
        {
            errno = 0;
            const dirent* item = ::readdir(stream);
            if (item == nullptr)
            {
                if (errno != 0)
                    result = FMINUS_FILE_FAILED;
                break;
            }
            if (sameLetters(item->d_name, segment))
            {
                result = FMINUS_FILE_OK;
                break;
            }
        }
        ::closedir(stream);
        return result;
    }

    namespace
    {
        // The check of a segment found beneath `directory`, where its file system calls for one (nameListed).
        std::int32_t ownName(Native directory, std::string_view segment)
        {
            return onePerEntry(static_cast<int>(directory)) ? FMINUS_FILE_OK : nameListed(directory, segment);
        }
    }   // namespace

    std::int32_t openDirectory(Native parent, std::string_view segment, Dir& out)
    {
        if (!plain(segment))
            return FMINUS_FILE_REFUSED;
        Dir dir;
        if (const std::int32_t status =
                opened(openAt(parent, std::string(segment), kWalk | O_DIRECTORY | kNoFollow | O_NONBLOCK), dir);
            status != FMINUS_FILE_OK)
            return status;
        // O_PATH with O_NOFOLLOW would open a link itself, were it not for O_DIRECTORY: what was opened is looked at.
        struct stat st;
        if (::fstat(static_cast<int>(dir.get()), &st) != 0)
            return statusOf(errno);
        if (!S_ISDIR(st.st_mode))
            return FMINUS_FILE_REFUSED;
        if (const std::int32_t status = ownName(parent, segment); status != FMINUS_FILE_OK)
            return status;
        out = std::move(dir);
        return FMINUS_FILE_OK;
    }

#if FMINUS_HAS_FILES_ANYWHERE
    std::int32_t realDirectory(const std::string& path, std::string& real)
    {
        if (path.empty() || path.front() != '/' || path.find('\0') != std::string::npos)
            return FMINUS_FILE_REFUSED;
        char* resolved = ::realpath(path.c_str(), nullptr);
        if (resolved == nullptr)
            return errno == ENOMEM ? FMINUS_FILE_FAILED : statusOf(errno);
        real = resolved;
        std::free(resolved);
        return FMINUS_FILE_OK;
    }

    std::int32_t openRoot(const std::string& real, Dir& out, std::size_t& rest)
    {
        if (real.empty() || real.front() != '/')
            return FMINUS_FILE_REFUSED;
        int fd = -1;
        do
            fd = ::open("/", kWalk | O_DIRECTORY | O_CLOEXEC);
        while (fd < 0 && errno == EINTR);
        rest = 1;
        return opened(fd, out);
    }
#endif

    std::size_t absolutePrefix(std::string_view name) noexcept
    {
        return !name.empty() && name.front() == '/' ? 1 : 0;
    }

    std::int32_t readFile(Native directory, std::string_view segment, const ReadRules& rules, unsigned char*& data,
                          std::int32_t& size)
    {
        data = nullptr;
        size = 0;
        if (!plain(segment) || rules.limit < 0)
            return FMINUS_FILE_REFUSED;
        const std::string name(segment);
        // Looked at before it is opened, so that only a regular file is ever opened: opening a device runs its driver.
        struct stat st;
        if (::fstatat(static_cast<int>(directory), name.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0)
            return statusOf(errno);
        if (!S_ISREG(st.st_mode))
            return FMINUS_FILE_REFUSED;
        if (const std::int32_t status = ownName(directory, name); status != FMINUS_FILE_OK)
            return status;
        const Fd file(openAt(directory, name, O_RDONLY | O_NOCTTY | O_NONBLOCK | kNoFollow));
        if (file.fd < 0)
            return statusOf(errno);
        // And looked at again once open: what the descriptor is, whatever was at the name a moment ago.
        if (::fstat(file.fd, &st) != 0)
            return statusOf(errno);
        if (!S_ISREG(st.st_mode) || deniedOne(rules.denied, identity(st)))
            return FMINUS_FILE_REFUSED;
        if (rules.singleLink && st.st_nlink > 1)
            return FMINUS_FILE_REFUSED;
#if defined(__linux__)
        if (kernelFileSystem(file.fd))
            return FMINUS_FILE_REFUSED;
#endif
        if (st.st_size > rules.limit)
            return FMINUS_FILE_LIMIT;
        // Read to the limit and one byte more: the size said is a hint (a file grows while it is read).
        const std::size_t most = static_cast<std::size_t>(rules.limit) + 1;
        std::size_t capacity = std::min<std::size_t>(static_cast<std::size_t>(st.st_size) + 1, most);
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
            const ssize_t n = ::read(file.fd, buffer + got, capacity - got);
            if (n < 0)
            {
                if (errno == EINTR)
                    continue;
                std::free(buffer);
                return FMINUS_FILE_FAILED;
            }
            if (n == 0)
                break;
            got += static_cast<std::size_t>(n);
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

    std::int32_t listDirectory(Native directory, Lister& lister)
    {
        // A descriptor of its own for reading the entries (readdir moves it, and the walk's may be O_PATH).
        const int fd = openAt(directory, ".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
        if (fd < 0)
            return statusOf(errno);
        DIR* stream = ::fdopendir(fd);
        if (stream == nullptr)
        {
            const int error = errno;
            ::close(fd);
            return error == ENOMEM ? FMINUS_FILE_FAILED : statusOf(error);
        }
        std::int32_t result = FMINUS_FILE_OK;
        for (;;)
        {
            errno = 0;
            const dirent* item = ::readdir(stream);
            if (item == nullptr)
            {
                if (errno != 0)
                    result = FMINUS_FILE_FAILED;
                break;
            }
            const std::string_view name(item->d_name);
            if (name == "." || name == ".." || !plain(name) || !lister.wanted(name))
                continue;
            Entry entry;
            entry.name = std::string(name);
            struct stat st;
            if (::fstatat(fd, item->d_name, &st, AT_SYMLINK_NOFOLLOW) == 0)
            {
                entry.regular = S_ISREG(st.st_mode);
                entry.links = static_cast<std::uint64_t>(st.st_nlink);
            }
            else if (errno != ENOENT)
            {
                result = statusOf(errno);
                break;
            }
            if (const std::int32_t status = lister.entry(entry); status != FMINUS_FILE_OK)
            {
                result = status;
                break;
            }
        }
        ::closedir(stream);
        return result;
    }

#if FMINUS_HAS_FILES_WRITE
    namespace
    {
        std::atomic<int> failNext{0};

        // Writes all of it, or fails.
        bool writeAll(int fd, const void* data, std::size_t bytes) noexcept
        {
            const auto* at = static_cast<const unsigned char*>(data);
            while (bytes > 0)
            {
                const ssize_t n = ::write(fd, at, bytes);
                if (n < 0)
                {
                    if (errno == EINTR)
                        continue;
                    return false;
                }
                if (n == 0)
                {
                    errno = EIO;
                    return false;
                }
                at += n;
                bytes -= static_cast<std::size_t>(n);
            }
            return true;
        }

        // Flushes a file to the disk: on macOS, fsync stops at the drive's cache, and F_BARRIERFSYNC orders it
        // (F_FULLFSYNC, which empties the cache, takes far longer).
        bool flush(int fd) noexcept
        {
#if defined(F_BARRIERFSYNC)
            if (::fcntl(fd, F_BARRIERFSYNC) == 0)
                return true;
#endif
            int result = 0;
            do
                result = ::fsync(fd);
            while (result != 0 && errno == EINTR);
            return result == 0;
        }

        // A name for a temporary file nobody else is using: hidden (a program cannot name it, a listing never shows
        // it), and made exclusively, so another one of the name is no harm but a retry.
        std::string temporaryName() noexcept
        {
            static std::atomic<std::uint64_t> counter{0};
            const std::uint64_t now =
                static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
            std::uint64_t x =
                now ^ (static_cast<std::uint64_t>(::getpid()) << 32) ^ (counter.fetch_add(1) * 0x9e3779b97f4a7c15ULL);
            x ^= x >> 31;
            x *= 0xbf58476d1ce4e5b9ULL;
            x ^= x >> 29;
            char name[40];
            std::snprintf(name, sizeof name, ".fminus-%016llx.tmp", static_cast<unsigned long long>(x));
            return name;
        }

        // What is at the target now: FILE_OK and `exists` (a regular file, not denied), or FILE_MISSING when nothing
        // is, or FILE_REFUSED for anything else.
        std::int32_t target(Native directory, const std::string& name, const WriteRules& rules, struct stat& st)
        {
            if (::fstatat(static_cast<int>(directory), name.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0)
                return statusOf(errno);
            if (!S_ISREG(st.st_mode) || deniedOne(rules.denied, identity(st)))
                return FMINUS_FILE_REFUSED;
            return ownName(directory, name);
        }

        // Whether `name` is one temporaryName makes: ".fminus-", 16 lower-case hexadecimal digits, ".tmp".
        bool temporaryShape(std::string_view name) noexcept
        {
            constexpr std::string_view head = ".fminus-";
            constexpr std::string_view tail = ".tmp";
            if (name.size() != head.size() + 16 + tail.size() || !name.starts_with(head) || !name.ends_with(tail))
                return false;
            for (const char c : name.substr(head.size(), 16))
                if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
                    return false;
            return true;
        }
    }   // namespace

    void sweepTemporaries(Native directory)
    {
        const int fd = openAt(directory, ".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
        if (fd < 0)
            return;
        DIR* stream = ::fdopendir(fd);
        if (stream == nullptr)
        {
            ::close(fd);
            return;
        }
        std::vector<std::string> found;
        for (;;)
        {
            const dirent* item = ::readdir(stream);
            if (item == nullptr)
                break;
            if (temporaryShape(item->d_name))
                found.emplace_back(item->d_name);
        }
        const std::int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
                                     std::chrono::system_clock::now().time_since_epoch())
                                     .count();
        for (const std::string& name : found)
        {
            struct stat st;
            if (::fstatat(fd, name.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0 || !S_ISREG(st.st_mode) ||
                st.st_nlink != 1 || static_cast<std::int64_t>(st.st_mtime) > now - kStaleTemporarySeconds)
                continue;
            ::unlinkat(fd, name.c_str(), 0);
        }
        ::closedir(stream);
    }

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
        const std::string name(segment);
        struct stat st;
        if (const std::int32_t status = target(directory, name, rules, st);
            status != FMINUS_FILE_OK && status != FMINUS_FILE_MISSING)
            return status;
        const int dir = static_cast<int>(directory);
        std::string temporary;
        int fd = -1;
        for (int attempt = 0; attempt < 16 && fd < 0; ++attempt)
        {
            temporary = temporaryName();
            if (fail == 1)
            {
                errno = EIO;
                break;
            }
            fd = openAt(directory, temporary, O_WRONLY | O_CREAT | O_EXCL | O_NOCTTY | kNoFollow, 0666);
            if (fd < 0 && errno != EEXIST)
                break;
        }
        if (fd < 0)
            return statusOf(errno);
        created = true;
        // From here on, any failure removes the temporary file, and the target is as it was.
        const auto failed = [&](int error, bool isOpen)
        {
            if (isOpen)
                ::close(fd);
            ::unlinkat(dir, temporary.c_str(), 0);
            return error == ENOSPC || error == EDQUOT || error == EFBIG || error == EIO ? FMINUS_FILE_FAILED
                                                                                        : statusOf(error);
        };
        if (fail == 2 || !writeAll(fd, data, static_cast<std::size_t>(bytes)))
            return failed(fail == 2 ? EIO : errno, true);
        if (fail == 3 || !flush(fd))
            return failed(fail == 3 ? EIO : errno, true);
        if (fail == 4)
        {
            ::close(fd);
            return failed(EIO, false);
        }
        if (::close(fd) != 0)
            return failed(errno, false);
        // The target once more, just before it is replaced: a directory there cannot be (rename says EISDIR); a
        // link or a FIFO put there meanwhile is replaced as a name, never followed.
        if (fail == 5 || ::renameat(dir, temporary.c_str(), dir, name.c_str()) != 0)
            return failed(fail == 5 ? EIO : errno, false);
        // The directory's entry, flushed: until then a crash could leave the old file (whole), never half of either.
        const Fd self(openAt(directory, ".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW));
        if (fail == 6 || self.fd < 0 || !flush(self.fd))
            return FMINUS_FILE_FAILED;   // the new file is in place, whole; whether it is on the disk is not known
        return FMINUS_FILE_OK;
    }

    std::int32_t appendFile(Native directory, std::string_view segment, const void* data, std::int32_t bytes,
                            const WriteRules& rules, bool& created)
    {
        created = false;
        if (!plain(segment) || bytes < 0 || (data == nullptr && bytes > 0))
            return FMINUS_FILE_REFUSED;
        if (bytes > rules.limit)
            return FMINUS_FILE_LIMIT;
        const std::string name(segment);
        int fd = -1;
        struct stat before;
        for (int attempt = 0; attempt < 2 && fd < 0; ++attempt)
        {
            const std::int32_t status = target(directory, name, rules, before);
            if (status == FMINUS_FILE_OK)
            {
                if (before.st_nlink > 1)
                    return FMINUS_FILE_REFUSED;   // a hard link: the bytes would land in a file elsewhere too
                fd = openAt(directory, name, O_WRONLY | O_APPEND | O_NOCTTY | O_NONBLOCK | kNoFollow);
                if (fd < 0 && errno != ENOENT)
                    return statusOf(errno);
            }
            else if (status == FMINUS_FILE_MISSING)
            {
                if (!rules.mayCreate)
                    return FMINUS_FILE_LIMIT;
                fd = openAt(directory, name, O_WRONLY | O_APPEND | O_CREAT | O_EXCL | O_NOCTTY | kNoFollow, 0666);
                if (fd >= 0)
                    created = true;
                else if (errno != EEXIST)
                    return statusOf(errno);
            }
            else
                return status;
        }
        if (fd < 0)
            return FMINUS_FILE_FAILED;   // the name changed under it twice
        const Fd file(fd);
        struct stat st;
        if (::fstat(fd, &st) != 0)
            return statusOf(errno);
        if (!S_ISREG(st.st_mode) || st.st_nlink > 1 || deniedOne(rules.denied, identity(st)))
            return FMINUS_FILE_REFUSED;
        if (!created && (st.st_dev != before.st_dev || st.st_ino != before.st_ino))
            return FMINUS_FILE_REFUSED;   // not the file that was looked at
#if defined(__linux__)
        if (kernelFileSystem(fd))
            return FMINUS_FILE_REFUSED;
#endif
        if (st.st_size + static_cast<off_t>(bytes) > static_cast<off_t>(rules.limit))
            return FMINUS_FILE_LIMIT;
        if (!writeAll(fd, data, static_cast<std::size_t>(bytes)))
            return errno == ENOSPC || errno == EDQUOT || errno == EFBIG || errno == EIO ? FMINUS_FILE_FAILED
                                                                                        : statusOf(errno);
        return FMINUS_FILE_OK;
    }
#endif
}   // namespace fminus::files::os
#endif
