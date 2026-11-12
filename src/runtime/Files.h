// src/runtime/Files.h: the file interface's host half (a build with FMINUS_FILES only; fminus/Config.h's
// FMINUS_HAS_FILES), in C++: the policy a host sets, its check, and the context a program's file builtins run under.
// The program's half is C, in runtime/Runtime.h (fminus_rt_file_*), listed in RuntimeSymbols.def. Both are part of the
// runtime library, with no LLVM and no dependency in them (docs/research/files/README.md is the design).
//
// The default is no files: a context whose policy grants no folder reaches no file, and a program given no context
// (null) reaches none either. A program names a file by a folder the host mapped and a path inside it
// ("presets/7.json"), never by where it is on the machine; in a build with FMINUS_FILES_ANYWHERE, and under a policy
// that says anywhere, it may also give an absolute name. What it can reach is decided from what was opened, never from
// a path's text:
//
//   - each folder's directory is opened once, when the context is made, and the grant is that handle: a folder renamed
//     afterwards is still the folder, and a directory put where it was is not;
//   - a name is checked in full by the runtime when it is used (its %d filled in first: the compiler's check of the
//     literal is a courtesy, this one the rule), then walked one directory at a time, each opened beneath the last
//     without following a symbolic link (or, on Windows, any reparse point), and no separator is ever in what the
//     system is given;
//   - each directory passed through, the folder's own and its ancestors among them, and the file itself, is compared
//     by identity (device and inode; on Windows the volume and the file id) with the denied directories: those found
//     when the context was made (held open, so one renamed after it still is), those found at their paths now, and
//     every one found at a denied path by an earlier look, so one created after the policy is denied from the first
//     operation that sees it there, wherever it goes next. The paths are looked at again once the walk is done and
//     once the file is read (or the directory listed), and an operation during which what is at one changed is
//     refused, so a directory swapped in or out of a denied path while a name is walked is never passed unseen. (One
//     made at a denied path after the policy and moved away before any operation looked is not known to be denied: it
//     takes someone who can write in a granted folder, who could copy its files as well.);
//   - each segment is the entry's own name: on a file system that also finds an entry by an 8.3 short name (FAT,
//     NTFS, a Windows share; FilesSystem.h, nameListed), "SSH~1" never reaches ".ssh";
//   - the file is opened without blocking and examined once open: a regular file, of a type the folder (or the
//     policy, or the build) allows, within the size limit, with one hard link when the policy asks, and on Linux not
//     on one of the kernel's own file systems (/proc, /sys); and it is read to the limit and one byte more into memory
//     that is allocated and filled before the program sees it, never mapped;
//   - an absolute name (anywhere) has its directory's links followed first, to find the real directory; the walk from
//     the root, its rules and the deny-list then apply to that real path, and the file itself is never a link.
//
// The names a program may give (the runtime refuses any other as FILE_REFUSED):
//
//   name    = folder "/" { segment "/" } file       a folder of the policy, then a path inside its directory
//           | "/" { segment "/" } file              absolute (on Windows, a drive letter, ":/", and the rest): a build
//                                                   with FMINUS_FILES_ANYWHERE and a policy that says anywhere only
//   segment = 1 to 255 bytes of UTF-8 (well formed: no overlong form, no surrogate, nothing past U+10FFFF), with no
//             control character (U+0000 to U+001F, U+007F to U+009F) and none of  \ : * ? " < > |  ; not beginning
//             with "." or a space, not ending with "." or a space; not a device name of Windows (CON, PRN, AUX, NUL,
//             COM and LPT with a digit, 0 to 9 or a superscript 1, 2 or 3, CONIN$, CONOUT$), in any letter case, with
//             or without an extension (what is before its first ".", spaces at its end aside)
//   file    = a segment with an extension: a "." and 1 to 16 ASCII letters and digits at its end (the last "."
//             counts: a.wav.exe is an exe), which the literal wrote, not a %d
//   At most 1024 bytes and 16 segments, the folder's among them, once the %d are filled in.
//
// A listing's pattern is a name whose file part has one "*" before its extension ("kit/*.f32", "bank%d/pad*.f32"):
// the "*" stands for any bytes, the fixed parts compare without regard to ASCII letter case. A listing is a snapshot of
// the folder's regular files whose names fit, each a name a program could have written (the rules above) of a type the
// folder allows, and (under singleLink) with one link, sorted by the bytes of their names. Links, directories and
// anything else are never listed.
//
// Writing (a build with FMINUS_FILES_WRITE, in a folder whose `write` is set; never an absolute name): file_write makes
// a temporary file beside the target, exclusively, through the directory's handle, writes it, flushes it, and renames
// it over the target, then (on Linux and macOS) flushes the directory; on any failure the temporary file is removed and
// the target is as it was. A process that dies half way leaves its temporary file (hidden, so no program can name it;
// on Windows it is marked to go with its handle until the rename): a context's first replacement in a directory
// removes those not written to for an hour. The target must be absent or a regular file: a link, a FIFO or a directory
// there is FILE_REFUSED. file_append adds to the end of a regular file with one link (a hard link would write a file
// elsewhere), creating it when it is absent; it promises nothing about a failure half way.
#pragma once

#include "fminus/Config.h"

#if FMINUS_HAS_FILES
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// A file builtin's status (Runtime.h, fminus_rt_file_status): FILE_OK, or why not. A program uses the names without
// declaring them; these are their values.
enum : std::int32_t
{
    FMINUS_FILE_OK = 0,
    FMINUS_FILE_MISSING = -1,   // no such file, or no such folder on disk
    FMINUS_FILE_REFUSED = -2,   // the policy: no folder of that name, a name the rules refuse, a denied directory, a
                                // type not allowed, a link on the way, something that is not a regular file (or a
                                // directory where one is walked through), a folder not granted for writing; or the
                                // program has no files at all (a null context: the default)
    FMINUS_FILE_DENIED = -3,    // the operating system said no: permissions, a sandbox
    FMINUS_FILE_LIMIT = -4,     // over a limit: the file's size, the files and listings held, the bytes held, a
                                // quota of bytes or files written, a listing's length
    FMINUS_FILE_FAILED = -5,    // the read or the write itself failed, or memory ran out
    FMINUS_FILE_INVALID = -6,   // no file: a null handle, a handle of another kind, an index past a listing, a
                                // negative size
};

namespace fminus::files
{
    // What a host allows a program. The default allows nothing: no folder, no files.
    struct Policy
    {
        struct Folder
        {
            std::string name;                 // what a program's name begins with: "presets" (1 to 64 ASCII letters,
                                              // digits, "_" and "-", not beginning with "-"; unique, letter case aside)
            std::string directory;            // where that is on this machine (opened once, links in it followed:
                                              // the host's own text)
            bool write = false;               // file_write and file_append may work here (FMINUS_FILES_WRITE)
            std::vector<std::string> types;   // the extensions allowed here (without the dot, any letter case);
                                              // empty: the policy's
        };
        std::vector<Folder> folders;      // the allow-list; none, the default: no files
        std::vector<std::string> deny;    // directories no name may lie in or pass through (and files no name may
                                          // reach), compared by identity; each looked up afresh at every open, and
                                          // what was ever found there denied for the context's life
        std::vector<std::string> types;   // the extensions allowed; empty: any the build allows
        bool anywhere = false;            // absolute names, wherever the deny-list and the types allow
                                          // (FMINUS_FILES_ANYWHERE); read and listed, never written
        bool singleLink = false;          // refuse a file with more than one hard link
        std::int32_t maxFileBytes = (256 << 20) < FMINUS_FILES_MAX_BYTES ? (256 << 20) : FMINUS_FILES_MAX_BYTES;
        // a file read, or written (1 to FMINUS_FILES_MAX_BYTES)
        std::int32_t maxOpen = 64;                           // files and listings a program holds at once (1 to 65536)
        std::int64_t maxHeldBytes = std::int64_t{1} << 30;   // bytes of files read and not yet closed (0 to 2^48)
        std::int64_t maxWrittenBytes = std::int64_t{1} << 30;   // bytes a program writes in its context's life
                                                                // (0 to 2^48)
        std::int32_t maxFilesWritten = 1024;   // files it writes in its context's life: each file_write, and each
                                               // file_append that creates its file (0 to 2^31 - 1)
        std::int32_t maxListed = 4096;         // a listing's files; more is FILE_LIMIT (1 to 1048576)
    };

    // Whether `policy` is one a context can take: an empty string, or what is wrong with it (the first mistake, the
    // field and the value quoted). It refuses whatever the build's ceilings do not allow: a type outside
    // FMINUS_FILES_TYPES, anywhere without FMINUS_FILES_ANYWHERE, a folder for writing without FMINUS_FILES_WRITE, a
    // maxFileBytes over FMINUS_FILES_MAX_BYTES. It reads no file: Context::create opens the folders.
    std::string checkPolicy(const Policy& policy);

    // A host's policy, its folders' directories opened, and the files and listings a program holds under it. Any
    // number of threads may use one context at once (its quotas and its list of handles take a lock).
    class Context
    {
    public:
        // A context for `policy`, or null with `error` set: what checkPolicy finds wrong, or a folder whose directory
        // cannot be opened as one ("folders: 'kit': '/x/samples' cannot be opened: No such file or directory").
        // Opens each folder's directory once, and each denied path that exists now; keeps every handle until it goes.
        static std::unique_ptr<Context> create(const Policy& policy, std::string& error);

        // Frees every file and listing still held under it, closed or not, and closes the folders' handles. No thread
        // may use it, or a handle it gave, once this has begun.
        ~Context();

        Context(const Context&) = delete;
        Context& operator=(const Context&) = delete;

        // What fminus_rt_file_read, _list, _write and _append take: the context, opaque.
        void* handle() noexcept;

        // Files and listings held now (for tests and hosts: what maxOpen counts), and the bytes of the files held.
        std::int32_t held() const noexcept;
        std::int64_t heldBytes() const noexcept;

        struct State;

    private:
        explicit Context(std::unique_ptr<State> state);
        std::unique_ptr<State> state_;
    };
}   // namespace fminus::files
#endif
