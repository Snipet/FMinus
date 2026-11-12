// src/web/ServerRoot.cpp: a directory's files as a server's static content (Web.h, readServerRoot), read once when a
// program that has no host starts: the driver's --web-root, an executable's FMINUS_WEB_ROOT. The server itself never
// maps a path to a file (Web.h, rule 4: it serves the bytes its policy holds), so nothing here runs while it serves,
// and nothing a request says can reach the file system. A build without FMINUS_WEB_SERVER has none of it.
#include "fminus/Config.h"

#if FMINUS_HAS_WEB_SERVER

#include "web/Policy.h"
#include "web/Web.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <fstream>
#else
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace fminus::web
{
    namespace
    {
        constexpr std::size_t kMaxFiles = 256;
        constexpr std::uintmax_t kMaxBytes = std::uintmax_t{16} << 20;
        constexpr std::size_t kMaxEntries = 4096;

        // The type of a file by its extension (in any letter case): a page's own, and a few of a page's assets.
        struct Extension
        {
            std::string_view extension;
            std::string_view type;
        };
        constexpr std::array kTypes{
            Extension{"html", "text/html; charset=utf-8"},
            Extension{"js", "text/javascript; charset=utf-8"},
            Extension{"mjs", "text/javascript; charset=utf-8"},
            Extension{"css", "text/css; charset=utf-8"},
            Extension{"json", "application/json"},
            Extension{"svg", "image/svg+xml"},
            Extension{"png", "image/png"},
            Extension{"jpg", "image/jpeg"},
            Extension{"gif", "image/gif"},
            Extension{"ico", "image/x-icon"},
            Extension{"wasm", "application/wasm"},
            Extension{"txt", "text/plain; charset=utf-8"},
            Extension{"wav", "audio/wav"},
        };

        std::string lowered(std::string_view text)
        {
            std::string out(text);
            for (char& c : out)
            {
                if (c >= 'A' && c <= 'Z')
                    c = static_cast<char>(c - 'A' + 'a');
            }
            return out;
        }

        std::string typeOf(std::string_view name)
        {
            const std::size_t dot = name.rfind('.');
            if (dot != std::string_view::npos)
            {
                const std::string extension = lowered(name.substr(dot + 1));
                for (const Extension& known : kTypes)
                {
                    if (known.extension == extension)
                        return std::string(known.type);
                }
            }
            return "application/octet-stream";
        }

        std::string inQuotes(std::string_view text)
        {
            return "'" + std::string(text) + "'";
        }

        // What a walk of the root has found so far.
        struct Walk
        {
            const std::string& directory;
            std::vector<ServerPolicy::File> read;
            std::vector<std::string> notes;
            std::uintmax_t total = 0;
            std::size_t entries = 0;
        };

        enum class Kind
        {
            Directory,
            Regular,
            Link,
            Other
        };
        enum class Take
        {
            Skip,
            Descend,
            Read
        };

        // The rules every entry meets, wherever it was found: counted, then skipped with a note (a link, a hidden
        // name, a path the server could not serve, anything but a directory or a regular file), descended into, or
        // read. Returns what is wrong, or nothing.
        std::string judge(Walk& walk, const std::string& relative, const std::string& name, Kind kind, Take& take)
        {
            take = Take::Skip;
            if (++walk.entries > kMaxEntries)
                return inQuotes(walk.directory) + " has more than " + std::to_string(kMaxEntries) + " entries";
            const auto skip = [&](std::string_view why)
            { walk.notes.push_back(inQuotes(relative) + ": " + std::string(why)); };
            if (kind == Kind::Link)
                skip("a symbolic link, which is never followed");
            else if (name.starts_with("."))
                skip("hidden");
            else if (!servablePath("/" + relative))
                skip("not a path the server serves (each name letters, digits, '-', '.', '_' and '~')");
            else if (kind == Kind::Directory)
                take = Take::Descend;
            else if (kind != Kind::Regular)
                skip("not a regular file");
            else
                take = Take::Read;
            return {};
        }

        // Room for one more file of `size` bytes, asked before it is read.
        std::string room(Walk& walk, std::uintmax_t size)
        {
            if (walk.read.size() == kMaxFiles)
                return inQuotes(walk.directory) + " has more than " + std::to_string(kMaxFiles) + " files to serve";
            walk.total += size;
            if (walk.total > kMaxBytes)
                return inQuotes(walk.directory) + " has more than 16 MiB of files to serve";
            return {};
        }

        void keep(Walk& walk, const std::string& relative, const std::string& name, std::string bytes)
        {
            const std::string path = "/" + relative;
            ServerPolicy::File file{path, typeOf(name), std::move(bytes)};
            if (name == "index.html")   // also at its directory's path: "/" or "/app"
            {
                const std::size_t slash = path.rfind('/');
                walk.read.push_back({slash == 0 ? "/" : path.substr(0, slash), file.type, file.bytes});
            }
            walk.read.push_back(std::move(file));
        }

#ifdef _WIN32
        // Windows: listed with std::filesystem and each file then read by its path. A symbolic link or a junction is
        // refused when it is listed, but a directory swapped for one between the listing and the read would be
        // followed (there is no openat): the root must be writable by no one else while the program starts. Never
        // built or run.
        std::string walkRoot(Walk& walk)
        {
            namespace fs = std::filesystem;
            std::error_code error;
            const fs::path root(walk.directory);
            if (!fs::is_directory(root, error))
                return inQuotes(walk.directory) + " is not a directory";
            fs::recursive_directory_iterator it(root, fs::directory_options::none, error);
            if (error)
                return inQuotes(walk.directory) + " cannot be read: " + error.message();
            for (; it != fs::recursive_directory_iterator(); it.increment(error))
            {
                if (error)
                    return inQuotes(walk.directory) + " cannot be read: " + error.message();
                const fs::directory_entry& entry = *it;
                const std::string relative = entry.path().lexically_relative(root).generic_string();
                const std::string name = entry.path().filename().string();
                const fs::file_status status = entry.symlink_status(error);
                if (error)
                    return inQuotes(relative) + " cannot be read: " + error.message();
                const Kind kind = fs::is_symlink(status)        ? Kind::Link
                                  : fs::is_directory(status)    ? Kind::Directory
                                  : fs::is_regular_file(status) ? Kind::Regular
                                                                : Kind::Other;
                Take take = Take::Skip;
                if (std::string wrong = judge(walk, relative, name, kind, take); !wrong.empty())
                    return wrong;
                if (take == Take::Skip && kind == Kind::Directory)
                    it.disable_recursion_pending();
                if (take != Take::Read)
                    continue;
                const std::uintmax_t size = entry.file_size(error);
                if (error)
                    return inQuotes(relative) + " cannot be read: " + error.message();
                if (std::string wrong = room(walk, size); !wrong.empty())
                    return wrong;
                std::string bytes(static_cast<std::size_t>(size), '\0');
                std::ifstream in(entry.path(), std::ios::binary);
                in.read(bytes.data(), static_cast<std::streamsize>(size));
                if (!in || in.gcount() != static_cast<std::streamsize>(size) ||
                    in.peek() != std::ifstream::traits_type::eof())
                    return inQuotes(relative) + " cannot be read";
                keep(walk, relative, name, std::move(bytes));
            }
            return {};
        }
#else
        std::string beneath(const std::string& relative, const std::string& name)
        {
            return relative.empty() ? name : relative + "/" + name;
        }

        std::string failure(const std::string& what)
        {
            return inQuotes(what) + " cannot be read: " + std::generic_category().message(errno);
        }

        // The regular file `name` in the directory open as `at`, `size` bytes of it, without following a link; false
        // when it cannot be read, is no longer a regular file, or is not that size.
        bool readAt(int at, const std::string& name, std::uintmax_t size, std::string& bytes)
        {
            bytes.assign(static_cast<std::size_t>(size), '\0');
            const int fd = ::openat(at, name.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
            if (fd < 0)
                return false;
            struct stat info{};
            bool fine = ::fstat(fd, &info) == 0 && S_ISREG(info.st_mode) &&
                        static_cast<std::uintmax_t>(info.st_size) == size;
            std::size_t done = 0;
            while (fine && done < bytes.size())
            {
                const ssize_t got = ::read(fd, bytes.data() + done, bytes.size() - done);
                if (got < 0 && errno == EINTR)
                    continue;
                if (got <= 0)
                    fine = false;
                else
                    done += static_cast<std::size_t>(got);
            }
            ::close(fd);
            return fine;
        }

        // The directory open as `at` (which this closes) and everything beneath it; `relative` is its path in the
        // root, empty for the root. Each directory is opened beneath the one that listed it with O_NOFOLLOW and held
        // open while its entries are looked at and read with openat: a path is never resolved again from the root,
        // so a directory swapped for a link after it was listed is the link refused, or the directory itself, and
        // never somewhere else.
        std::string walkDirectory(Walk& walk, int at, const std::string& relative)
        {
            std::vector<std::string> names;
            {
                const int listing = ::dup(at);
                DIR* const dir = listing < 0 ? nullptr : ::fdopendir(listing);
                if (dir == nullptr)
                {
                    std::string wrong = failure(relative.empty() ? walk.directory : relative);
                    if (listing >= 0)
                        ::close(listing);
                    ::close(at);
                    return wrong;
                }
                errno = 0;
                while (const dirent* const entry = ::readdir(dir))
                {
                    const std::string_view name(entry->d_name);
                    if (name != "." && name != "..")
                        names.emplace_back(name);
                    if (names.size() > kMaxEntries)
                        break;
                }
                const int error = errno;
                ::closedir(dir);
                if (error != 0)
                {
                    errno = error;
                    std::string wrong = failure(relative.empty() ? walk.directory : relative);
                    ::close(at);
                    return wrong;
                }
            }
            std::ranges::sort(names);
            std::string wrong;
            for (const std::string& name : names)
            {
                const std::string path = beneath(relative, name);
                struct stat info{};
                if (::fstatat(at, name.c_str(), &info, AT_SYMLINK_NOFOLLOW) != 0)
                {
                    wrong = failure(path);
                    break;
                }
                const Kind kind = S_ISLNK(info.st_mode)   ? Kind::Link
                                  : S_ISDIR(info.st_mode) ? Kind::Directory
                                  : S_ISREG(info.st_mode) ? Kind::Regular
                                                          : Kind::Other;
                Take take = Take::Skip;
                if (wrong = judge(walk, path, name, kind, take); !wrong.empty())
                    break;
                if (take == Take::Descend)
                {
                    const int inner = ::openat(at, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
                    if (inner < 0)
                        wrong = failure(path);
                    else
                        wrong = walkDirectory(walk, inner, path);
                }
                else if (take == Take::Read)
                {
                    const auto size = static_cast<std::uintmax_t>(info.st_size);
                    if (wrong = room(walk, size); wrong.empty())
                    {
                        std::string bytes;
                        if (readAt(at, name, size, bytes))
                            keep(walk, path, name, std::move(bytes));
                        else
                            wrong = inQuotes(path) + " cannot be read";
                    }
                }
                if (!wrong.empty())
                    break;
            }
            ::close(at);
            return wrong;
        }

        // POSIX: the root itself is opened as named (a link the host names is the host's choice), and everything
        // beneath it by descriptor.
        std::string walkRoot(Walk& walk)
        {
            const int root = ::open(walk.directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
            if (root < 0)
            {
                if (errno == ENOENT || errno == ENOTDIR)
                    return inQuotes(walk.directory) + " is not a directory";
                return failure(walk.directory);
            }
            return walkDirectory(walk, root, {});
        }
#endif
    } // namespace

    std::string readServerRoot(const std::string& directory, std::vector<ServerPolicy::File>& files,
                               std::vector<std::string>& skipped)
    {
        if (directory.empty())
            return "'' is not a directory";
        Walk walk{directory, {}, {}, 0, 0};
        if (std::string wrong = walkRoot(walk); !wrong.empty())
            return wrong;
        std::ranges::sort(walk.read,
                          [](const ServerPolicy::File& a, const ServerPolicy::File& b) { return a.path < b.path; });
        files = std::move(walk.read);
        skipped.insert(skipped.end(), walk.notes.begin(), walk.notes.end());
        return {};
    }
} // namespace fminus::web

#endif
