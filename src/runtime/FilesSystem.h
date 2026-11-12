// src/runtime/FilesSystem.h: what the file interface (Files.cpp) asks of the operating system, one directory or one
// file at a time: FilesPosix.cpp for Linux and macOS, FilesWindows.cpp for Windows. Files.cpp checks names, policies
// and quotas and never opens anything itself; these functions open, examine, read, list and write, and never read a
// path's text from a program except as one segment, which never holds a separator. Each returns a status of Files.h's
// (FMINUS_FILE_OK or why not). A build without FMINUS_FILES has none of this, and one without FMINUS_FILES_WRITE has no
// function that creates, writes or renames a file.
#pragma once

#include "runtime/Files.h"

#if FMINUS_HAS_FILES
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace fminus::files::os
{
    // A file descriptor, or a Windows HANDLE, as an integer; kNone for none.
    using Native = std::intptr_t;
    inline constexpr Native kNone = -1;

    // An open directory (or, for a denied path, whatever it is), closed when this goes.
    class Dir
    {
    public:
        Dir() = default;
        explicit Dir(Native native) noexcept
            : native_(native)
        {
        }
        ~Dir();
        Dir(Dir&& other) noexcept
            : native_(other.native_)
        {
            other.native_ = kNone;
        }
        Dir& operator=(Dir&& other) noexcept;
        Dir(const Dir&) = delete;
        Dir& operator=(const Dir&) = delete;

        Native get() const noexcept
        {
            return native_;
        }
        bool valid() const noexcept
        {
            return native_ != kNone;
        }

    private:
        Native native_ = kNone;
    };

    // What a file or directory is, whatever its names: its device and inode (POSIX), or its volume's serial number and
    // its 128-bit file id (Windows).
    struct Identity
    {
        std::uint64_t volume = 0;
        std::uint64_t high = 0;
        std::uint64_t low = 0;
        friend bool operator==(const Identity&, const Identity&) = default;
    };

    // The host's directory for a folder, opened by its path (links in it followed: it is the host's text): FILE_OK,
    // or why not with `why` saying it (the system's words).
    std::int32_t openFolder(const std::string& path, Dir& out, std::string& why);

    // A denied path, opened by its path (links followed) to pin its identity for the context's life: FILE_OK, or
    // FILE_MISSING when nothing is there now.
    std::int32_t openDenied(const std::string& path, Dir& out);

    // What `native` (a Dir's) is.
    std::int32_t identityOf(Native native, Identity& out);

    // What is at `path` now (links followed): FILE_OK, or FILE_MISSING when nothing is, or another failure.
    std::int32_t lookUp(const std::string& path, Identity& out);

    // The identities of the directories `directory` lies in, from its parent to the root, as they are now (POSIX: by
    // "..", which no link can bend; Windows: the final path of the handle, each prefix opened).
    std::int32_t ancestors(Native directory, std::vector<Identity>& out);

    // Opens the directory `segment` beneath `parent`, following no link (nor any reparse point), without blocking:
    // FILE_OK; FILE_MISSING when nothing is there; FILE_REFUSED for a link or anything that is not a directory, or a
    // name the file system answers to that is not the entry's own (see nameListed).
    std::int32_t openDirectory(Native parent, std::string_view segment, Dir& out);

    // Whether `segment` is one of `directory`'s entries as the directory lists them, ASCII letter case aside: FILE_OK,
    // FILE_REFUSED when it is not, or why the directory could not be read. Some file systems find an entry by a second
    // name: FAT's and NTFS's 8.3 short names ("SSH~1" for ".ssh", "NOTES~1.WAV" for "notes.wavx"), and whatever a
    // Windows share, WSL's drives or a FUSE server answers to. A name's rules (no hidden file, the type its extension
    // says) hold only for the entry's own name, so on any file system not known to have one name per entry (POSIX: the
    // file systems of Linux and macOS listed in FilesPosix.cpp), every segment opened is checked this way; on Windows,
    // the opened object's long name is compared with the segment instead (GetFinalPathNameByHandleW).
    std::int32_t nameListed(Native directory, std::string_view segment);

    // For tests only: checks every segment with nameListed, whatever the file system (true), or only where names may
    // have a second form (false, the default). It can only add a check.
    void checkEveryName(bool every) noexcept;

#if FMINUS_HAS_FILES_ANYWHERE
    // The absolute directory `path` ("/", or "C:/" on Windows, then segments separated by "/") with every link in it
    // followed, as an absolute path of the same form: what an absolute name's directory really is (anywhere only).
    std::int32_t realDirectory(const std::string& path, std::string& real);

    // The root a real directory's path begins with ("/", or "C:/"), opened; `rest` is the length of that prefix.
    std::int32_t openRoot(const std::string& real, Dir& out, std::size_t& rest);
#endif

    // How long an absolute name's root prefix is: 1 for "/" on POSIX, 3 for "C:/" on Windows; 0 when `name` is not
    // absolute here.
    std::size_t absolutePrefix(std::string_view name) noexcept;

    // What a file to read must be, besides a regular file.
    struct ReadRules
    {
        std::int32_t limit = 0;                          // bytes, at most
        bool singleLink = false;                         // one hard link only
        const std::vector<Identity>* denied = nullptr;   // what the file itself may not be
    };

    // Opens `segment` beneath `directory` without following a link and without blocking, examines what was opened
    // (a regular file; not denied; links; not on a file system of the kernel's own; its size), and reads it whole into
    // memory from malloc, read to the limit and one byte more, with a 0 byte after its last (not counted): FILE_OK with
    // `data` and `size` set (the caller frees `data`), or why not with nothing to free.
    std::int32_t readFile(Native directory, std::string_view segment, const ReadRules& rules, unsigned char*& data,
                          std::int32_t& size);

    // A directory's entry, as a listing sees it without opening it.
    struct Entry
    {
        std::string name;
        bool regular = false;      // a regular file (never a link to one)
        std::uint64_t links = 0;   // its hard links
    };

    // Reads the names of `directory`'s entries (not "." or ".."; UTF-8 on every system, a name that is not, on
    // Windows, skipped), asking wanted(name) of each, and for each wanted one calling entry() with its `regular` and
    // `links` (looked at beneath the directory, following no link: an entry gone meanwhile is not regular). entry()
    // returns FILE_OK to go on, or a status to stop with, which this returns.
    struct Lister
    {
        virtual ~Lister() = default;
        virtual bool wanted(std::string_view name) = 0;
        virtual std::int32_t entry(const Entry& entry) = 0;
    };
    std::int32_t listDirectory(Native directory, Lister& lister);

#if FMINUS_HAS_FILES_WRITE
    // What a write must keep to.
    struct WriteRules
    {
        std::int32_t limit = 0;                          // the file's bytes, at most (an append: its new size)
        const std::vector<Identity>* denied = nullptr;   // what the target may not be
        bool mayCreate = true;                           // an append may create its file (else: FILE_LIMIT)
    };

    // Replaces `segment` beneath `directory` with the `bytes` bytes at `data`, whole or not at all (Files.h). `created`
    // is set once a temporary file exists (what counts as a file written, success or not).
    std::int32_t replaceFile(Native directory, std::string_view segment, const void* data, std::int32_t bytes,
                             const WriteRules& rules, bool& created);

    // Adds the `bytes` bytes at `data` at the end of `segment` beneath `directory`, creating it (`created` set) when it
    // is absent (Files.h).
    std::int32_t appendFile(Native directory, std::string_view segment, const void* data, std::int32_t bytes,
                            const WriteRules& rules, bool& created);

    // Removes from `directory` the temporary files replaceFile left when its process died half way: regular files
    // with one link named ".fminus-" and 16 lower-case hexadecimal digits and ".tmp", not written to for an hour (one
    // younger may be another process's, still being written). Nothing is opened or followed: each is looked at and
    // removed by its name beneath the directory. Failures are ignored; nothing a program can name is touched (the
    // names are hidden).
    void sweepTemporaries(Native directory);

    // How long a temporary file is left alone, in seconds, before sweepTemporaries takes it for a dead write's.
    inline constexpr std::int64_t kStaleTemporarySeconds = 3600;

    // For tests only: makes the next replaceFile (any thread's) fail at `step` as if the system had (1: creating the
    // temporary file, 2: writing it, 3: flushing it, 4: closing it, 5: renaming it, 6: flushing the directory);
    // 0: none. Each failure is taken once.
    void failNextReplaceAt(int step) noexcept;
#endif
}   // namespace fminus::files::os

#endif
