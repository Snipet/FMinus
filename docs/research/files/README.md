# A file system interface for FMinus programs

*Research, 2026-11-11, written against FMinus as the open pull request has it (string literals as builtins'
arguments, the parsers, the web library, and the server being built the same day). The question asked: how would
FMinus give its programs the file system, switchable when FMinus is built, with restrictions a host can set: which
folders, which types of file? Nothing under `src/`, `include/` or `cmake/` changed for this note. §11 says what was
read at its source and what was not.*

*Since then (2026-11-12), it is built as section 0 chose, all of it but what §10 leaves: [files.md](../../files.md) is
its manual and [extending.md](../../extending.md), recipe 43, its walk through the code, and
[examples/kit/](../../../examples/kit/README.md) the program of §4.1. Those pages say where what was built differs from
this report, chiefly: a failure is a shared handle, so `file_read` never gives null, not even when memory runs out;
`file_read_nth` past a listing is `FILE_INVALID`; a listing's pattern must write its extension (its `*` before the
last `.`), and more files than `maxListed` is `FILE_LIMIT` with no listing at all; a folder's name is 1 to 64 ASCII
letters, digits, `_` and `-`; the deny-list may name files, holds each denied path open as well as looking it up at
every open, remembers whatever it ever found at a path, refuses an operation during which what is at one changed, and
fails closed when a path cannot be looked up; a segment must be its directory's own name for an entry, not an 8.3
short name (on any file system but Linux's and macOS's own, and always on Windows); a write's temporary file that a
dead process left is removed by a later write once it is an hour old; a closed handle stays its context's memory, so
closing it twice is harmless on any allocator; `anywhere` reads and lists but never writes, and an
absolute name's real path must keep the segment rules; on Linux a file on one of the kernel's own file systems
(`/proc`, `/sys`, ...) is refused; C1 control characters, `COM0` and `LPT0` are refused too; `maxFileBytes` defaults to
256 MiB or the build's ceiling, whichever is less, and every limit has a range; a file is looked at before it is opened
as well as after, so a device is never opened when it is there to be seen; `openat2` is not used, on any kernel; on
Windows a file is replaced through `NtSetInformationFile`'s rename classes rather than `ReplaceFileW`; and there is no
macro for absolute names. The macOS and Windows code has never been built (the Windows file was type-checked against
stub headers only). The report is left as it was written.*

## 0. Summary

**What I would build:**

1. **Whole files, by literal names.** `file_read("presets/%d.json", n)` loads a file into memory and gives a handle;
   its bytes go to the builtins FMinus has (`json_parse`, `xml_parse`, `bytes_floats`, `bytes_pcm16`).
   `file_list("samples/*.f32")` snapshots a folder; `file_write` and `file_append` put bytes on disk. A name is a
   string literal, as a URL is, so **the host can list every file a program names before it runs** (§4).
2. **Folders the host grants, and nothing else.** The host maps names to directories (`presets` is
   `~/Music/Synth/Presets`, read-only, `.json` only); a program's name begins with one. A deny-list of directories
   overrides every grant. The default is no files at all (§6).
3. **Enforced on what was opened, not on what was written.** Each directory of a name is opened beneath the one
   before it, never through a symbolic link, and compared by identity against the denied ones; the file is checked
   after it is open (a regular file, its size, its links). No string of a path is ever trusted to say where a file
   is (§5).
4. **Decided when FMinus is built, narrowed when it runs.** Four build options say whether a build can read files,
   write them, be given names outside folders, and which types it can ever touch; a host's policy can only narrow
   what the build has (§7).
5. **Off the audio thread.** Every builtin that touches the disk allocates or blocks, and the effects refuse it in a
   `[[clang::nonblocking]]` function. The bytes of a file that was read are in memory, not mapped, and reading them
   is wait-free (§4.3).

**The four things that decide the design:**

- **FMinus has no strings.** A program cannot compute a file's name, read one from a preset, or show one. That is a
  restriction other languages would envy here: the set of names a program can ever open is in its source, and the
  compiler can check each one.
- **Path strings lie.** `..`, symbolic links, case-insensitive and normalising file systems, Windows device names
  and alternate streams have each broken permission systems that compared strings: Deno's and Node's file
  permissions among them, more than once (appendix B). The fix is the same everywhere it has been fixed: open
  relative to a directory handle, refuse links, and decide from the open file.
- **The file system is slow, and unpredictable in how slow.** No file builtin may run on the audio thread, and a
  memory-mapped file is no way round it: a page fault is disk I/O on the thread that took it.
- **The honest limit is the web library's.** The policy bounds FMinus's file builtins, not the process: a program
  can still declare the C library's own `open` and call it, as [web.md](../../web.md) says of `socket`. Closing
  that is one change to the language's host interface, for both.

**Decisions that are yours, and what I chose so the work can go on:**

| Decision | Chosen | The alternative |
|---|---|---|
| What a program names | a folder the host mapped, then a path inside it | a path relative to one base directory; or absolute paths |
| Default at run time | no files | everything, with restrictions opt-in |
| Default build | reading on; writing and names outside folders off | all on; or all off, as the web library is |
| Symbolic links | refused wherever they are met | followed when they stay inside the folder |
| Types | by a name's extension, a list of those allowed | by content as well ("magic numbers") |
| Listing a folder | in the first version | later |
| Where the code lives | in the runtime library (`src/runtime/`), as the parsers are | a library of its own, as the web's is |
| Decoders (WAV, AIFF, FLAC) | not here: files give bytes | part of this work |

## 1. What a program would do with files

| Use | Shape | Needs |
|---|---|---|
| Load data before playing: wavetables, impulse responses, sample sets, tuning tables, presets | one read each, off the audio thread, then parse or decode | read a whole file; the parsers; `bytes_floats` |
| Load "what is in the folder": every sample of a kit, every preset of a bank | a listing, then a read each | enumeration without names |
| Save: a preset, a rendered table, a bounce, a log of levels | a whole file written, or bytes appended | write, replace atomically, append |
| Play a file too long for memory | a reader thread and a ring of blocks | streaming: not the first version (§10) |
| Watch a file and reload it when it changes | a timestamp, polled | not the first version |

The first row is nearly all of the value, as it was for the web library, and it has the same shape: **load, then
run**. FMinus's determinism survives it whole: the output depends on the files' bytes, not on when they arrived.

## 2. What FMinus has that this builds on

- **String literals as builtins' arguments**, with `%d` places for ints, checked and recorded when the program is
  compiled: a file's name is one more kind, after formats, JSON Pointers, XML paths, URLs and routes.
- **Three effect classes**, checked as errors: a builtin that touches the disk is the class that allocates, refused
  in nonblocking and nonallocating functions alike.
- **The pattern for a feature a build can leave out**: an option, `FMINUS_HAS_…` in `fminus/Config.h`, a predefined
  macro, builtins that stay known names with a "this build of FMinus leaves out" error, a lit feature.
- **The pattern for a host's permission**: a policy in `Program::Options`, a context the program's module holds in
  a global it cannot name, a listing of what the program will ask for (`webRequests()`), options for the driver and
  an environment for executables, default-deny throughout.
- **Build-time limits already exist**: the parsers' largest document and deepest nesting are constants of the build
  (`FMINUS_JSON_MAX_BYTES`).
- **Everything that turns bytes into numbers**: `json_parse`, `xml_parse`, `bytes_floats`, `bytes_pcm16`. A file
  interface needs to deliver bytes and nothing more.
- **`#include` already reads files**, through a resolver the host supplies. It is the compiler's, at compile time,
  and stays apart: what a program may include and what it may open while running are different permissions.

## 3. Where it lives

**In the runtime library**, `src/runtime/`, as the parsers are: it is what generated code calls, it has no LLVM in
it, an executable links it already, and the file interface needs no dependency at all. The web library is its own
library because libcurl is; nothing here is.

The host's half (the policy, the context) is C++ beside it, as `src/web/Web.h` has two halves. `fminus_lib` makes a
context from `Program::Options::files`; an executable makes one from its environment before `main`.

## 4. The program's side

### 4.1 The builtins

```c
module Kit
{
    float sample[16][48000];
    int length[16];
    int count;

    void load()                                             // off the audio thread: the effects insist
    {
        void* kit = file_list("kit/*.f32");                 // a snapshot of the folder, in name order
        count = file_count(kit);
        for (int i = 0; i < count && i < 16; ++i)
        {
            void* f = file_read_nth(kit, i);
            length[i] = bytes_floats(file_data(f), file_size(f), sample[i], 48000);
            file_close(f);
        }
        file_close(kit);
    }

    int save(int slot)                                      // a build with FMINUS_FILES_WRITE
    {
        return file_write("user/take%d.f32", sample[slot], 4 * length[slot], slot);
    }
};
```

| Builtin | Does | Effect |
|---|---|---|
| `void* file_read("name", ...)` | opens the file under the host's policy, reads all of it into memory, closes it, and gives a handle | allocates |
| `int file_status(h)` | `FILE_OK`, or why not (below) | wait-free |
| `int file_size(f)`, `const void* file_data(f)` | the bytes read and their address, until `file_close`; 0 and null for a file that was not read | wait-free |
| `void* file_list("folder/*.ext", ...)` | the regular files of one folder whose names fit the pattern, in name order, as of now | allocates |
| `int file_count(list)` | how many | wait-free |
| `void* file_read_nth(list, i)` | reads the i-th, as `file_read` does | allocates |
| `void file_close(h)` | frees a file's bytes, or a listing | allocates |
| `int file_write("name", data, bytes, ...)` | writes the bytes as the whole file, replacing it in one step or not at all | allocates |
| `int file_append("name", data, bytes, ...)` | adds the bytes at the file's end, creating it if need be | allocates |

- **A name is a string literal**, a format whose `%d`s are the ints after every other argument, as a URL's are.
  It is checked when the program is compiled (§4.2) and recorded: `Program::files()` lists every one with its
  operation, its pattern (each `%d` a `*`) and where it is written.
- **A handle is an opaque `void*`** that knows its kind, as the web library's now do. A file that could not be read
  is a handle too, its status saying why: `file_read` gives null only when memory ran out. A null handle reads as
  `FILE_INVALID`, 0 and null.
- **The status codes** are names a program uses without declaring them, as `HTTP_REFUSED` is:

  | Name | Value | Means |
  |---|---|---|
  | `FILE_OK` | 0 | done |
  | `FILE_MISSING` | -1 | no such file, or no such folder on disk |
  | `FILE_REFUSED` | -2 | the host's policy: no folder of that name, a denied directory, a type not allowed, a link on the way, something that is not a regular file; or the program has no files at all (the default) |
  | `FILE_DENIED` | -3 | the operating system said no: permissions, a sandbox |
  | `FILE_LIMIT` | -4 | over a limit: the file's size, the files open, the bytes held, a quota of bytes or files written, a listing's length |
  | `FILE_FAILED` | -5 | the read or the write itself failed, or memory ran out |
  | `FILE_INVALID` | -6 | no file: a null handle, a handle of another kind, a negative size |

- **A listing is a snapshot.** `file_list` reads the folder once; `file_count` and `file_read_nth` then mean the
  same files however the folder changes meanwhile (one that has gone is `FILE_MISSING` when read). Operating
  systems promise no order, so the order is FMinus's: by the names' bytes. A program never sees a name, only an
  index, and files whose names a program could not have written (§4.2) are not listed.
- **Writing replaces or does nothing.** `file_write` writes a temporary file beside the target, flushes it, and
  renames it over the target: a reader, or a crash, sees the old file or the new one, never half of either.
  `file_append` cannot promise that, and says so.

### 4.2 Names, and what the compiler refuses

```
name    = folder "/" { segment "/" } file          the folder is one the host mapped (§6)
        | absolute                                  only in a build with FMINUS_FILES_ANYWHERE
segment = 1 to 255 bytes of UTF-8, none of them a control byte or one of  \ : * ? " < > |
          not beginning with "." or a space, not ending with "." or a space,
          not a device name of Windows (CON, PRN, AUX, NUL, COM and LPT with a digit, the superscript digits
          among them, CONIN$, CONOUT$), with or without an extension, in any letter case
file    = a segment with an extension: a "." and 1 to 16 ASCII letters and digits at its end
```

Every rule is there because a file system somewhere reads the forbidden form as something else: `..` and `.`
climb or stand still; a leading `.` hides a file (and `.ssh`, `.env` and `.git` are what a careless grant exposes);
Windows strips a trailing dot or space, opens a device for `NUL.txt`, and reads `a.wav:x` as a stream of `a.wav`;
`\` is a separator there and a letter elsewhere. Refusing them **on every system** keeps a program's meaning the
same on all three, and each is an error where it is written, in clang's manner, at its byte.

- **The extension is the file's type**, and must be in the literal: a `%d` may not be in it, so the compiler knows
  the type of every file a program will ever open, and refuses one the build cannot read (§7). Only the last
  extension counts (`a.wav.exe` is an `exe`), compared without regard to ASCII case.
- **A listing's pattern** is a name whose file part has one `*`: `"kit/*.f32"`, `"bank%d/pad*.f32"`. The `*`
  stands for any bytes within the name; the fixed parts compare without regard to ASCII case, so `*.wav` finds
  `KICK.WAV` on every system rather than only where the file system folds case.
- **Length:** 1024 bytes once its `%d`s are filled in, 16 segments: over either is `FILE_REFUSED` when it runs.

### 4.3 The audio thread

Opening, reading, writing and closing a file each wait on the disk for a time no one can bound, and the survey's
authorities say the same thing in the same words (appendix A): never on the thread that renders. So:

- **Everything that touches the disk allocates** in the effects' terms, refused in `[[clang::nonblocking]]` and
  `[[clang::nonallocating]]` functions. A program loads in `init` or a method its host calls from a loader thread.
- **What was read is in memory, all of it, before the handle is returned**: allocated, read into, never mapped. A
  mapped file's first touch is a page fault, which is a read from the disk on whichever thread touched it. So
  `file_data` and `file_size` are plain loads, wait-free, and the audio thread may read a table straight from a
  file's bytes for as long as the handle is open.
- **Nothing here is asynchronous.** `http_get` returns at once because a request takes seconds and a thread already
  exists to wait for it. A file takes milliseconds and FMinus would need a thread to hide them: the host has
  one. Streaming a long file while it plays is the case that does need a thread, and is §10's.

## 5. Enforcement: the file that was opened

A permission check on a path's text asks the wrong question. What matters is the file the kernel will open for it,
and between a check and an open, that can change. Appendix A has the facts for each system; the design they lead to:

1. **Each granted folder is opened once**, when the policy is made, and FMinus keeps the handle. The grant is that
   handle, not the path it came from: if the directory is renamed, the grant follows it; if a sandbox takes the
   access away, opens fail (`FILE_DENIED`) and nothing widens.
2. **A name is walked one directory at a time**, each opened beneath the last, with the system's "do not follow a
   link" flag, so no segment can be a symbolic link, a junction or any other reparse point, and none can climb
   (the compiler already refused `..`). This is what Go's `os.Root` and Rust's cap-std do where the kernel has no
   single call for it, and it is where their bugs were: cap-std's walk passed a final segment to the kernel with a
   trailing slash, which made the kernel follow the link `O_NOFOLLOW` was there to refuse (2026, rated critical).
   So the walk here is as plain as it can be made: one segment a call, never a separator in what the kernel is
   given, and one function that every builtin goes through.
3. **Each directory passed through is compared by identity** (device and inode; volume and file id on Windows) with
   the denied directories. Identity, not spelling: `Private`, `private` and a differently normalised `Privé` are
   one directory on macOS and Windows, and two strings.
4. **The file is opened without blocking, then examined**: it must be a regular file (a FIFO would hang, a device
   would do who knows what), within the size limit, and, where the host asked, have a single hard link.
5. **It is read to the limit and one byte more.** A file's reported size is a hint (a file can grow while it is
   read, and some report zero): over the limit is `FILE_LIMIT`, and nothing partial is ever returned.
6. **A new file is created through its directory's handle**, exclusively, never by a path: so a write can only land
   in a directory that was just walked to and checked.

| | Linux | macOS | Windows |
|---|---|---|---|
| Open beneath a directory, no links | `openat` a segment at a time with `O_NOFOLLOW`; `openat2` with `RESOLVE_BENEATH` and `RESOLVE_NO_SYMLINKS` does it in one call from 5.6, where a sandbox lets it through | the same walk; `O_NOFOLLOW_ANY` (macOS 11) as a second check. `O_RESOLVE_BENEATH` exists only from 15.4, and earlier kernels appear to ignore it silently: not relied on | `NtCreateFile` with the folder's handle as `RootDirectory` and `OBJ_DONT_REPARSE`; a segment at a time where that is refused |
| Not a regular file | `O_NONBLOCK`, then `fstat` and `S_ISREG` | the same | `GetFileType` is `FILE_TYPE_DISK`, and not a directory |
| Identity | `st_dev`, `st_ino` | the same | the volume's serial number and the file's id |
| Replace a file in one step | temporary file, `fsync`, `renameat`, `fsync` of the directory | the same, with `F_BARRIERFSYNC` | `SetFileInformationByHandle` with `FileRenameInfoEx` and POSIX semantics, else `ReplaceFileW`, a few times over (other software holds files open there) |
| "Denied" | `EACCES`, `EPERM` | `EACCES` (permissions), `EPERM` (the sandbox, privacy controls) | `ERROR_ACCESS_DENIED` |

**What this does not stop**, said plainly:

- **A hard link** inside a granted folder to a file elsewhere. A program cannot make one (it has no builtin that
  links), so someone with write access to the folder did. A host that minds sets `singleLink`, and such a file is
  `FILE_REFUSED`.
- **A mount** beneath a granted folder is followed: it is a directory there.
- **The process.** The policy is the file builtins', as the web policy is the web library's: not a sandbox.

`std::filesystem` does none of this work. Its `canonical` and `lexically_normal` are operations on text, the
standard calls a race in its functions undefined behaviour, and its libraries differ on Windows. It may split a
path; it does not open one.

## 6. The host's side: folders, the deny-list, types

```cpp
struct FilePolicy                                 // Program::Options::files
{
    struct Folder
    {
        std::string name;                         // what a program's name begins with: "presets"
        std::string directory;                    // where that is on this machine
        bool write = false;                       // file_write and file_append may work here
        std::vector<std::string> types;           // the extensions allowed here; empty: the policy's
    };
    std::vector<Folder> folders;                  // the allow-list; none, the default: no files
    std::vector<std::string> deny;                // directories no name may lie in or pass through
    std::vector<std::string> types;               // the extensions allowed; empty: any the build allows
    bool anywhere = false;                        // absolute names, wherever deny allows (§7)
    bool singleLink = false;                      // refuse a file with more than one hard link
    std::int32_t maxFileBytes = 256 << 20;        // a file read, or written
    std::int32_t maxOpen = 64;                    // files and listings a program holds at once
    std::int64_t maxHeldBytes = 1 << 30;          // bytes of files read and not yet closed
    std::int64_t maxWrittenBytes = 1 << 30;       // bytes a program writes in its life
    std::int32_t maxFilesWritten = 1024;          // files it creates in its life
    std::int32_t maxListed = 4096;                // a listing's files
};
```

- **The allow-list is a mapping, not a list of paths.** A program says `presets/7.json`; the host says where
  `presets` is on this machine, whether it can be written to and what may be read there. So a program carries no
  path of the machine it was written on, the same source runs in a plugin (the bundle's resources, the user's
  library) and under the driver (a directory given on the command line), and a folder the host did not map is
  refused whatever is on disk. This is WASI's preopened directories and JSFX's resource folders, and the opposite
  of the path lists that Deno and Node compare strings against (appendix B).
- **The deny-list wins**, always: a denied directory inside a granted folder is closed, however the name reaches
  it. It is looked up afresh at each open, so a directory made after the policy is denied too.
- **Types are an allow-list of extensions**, never a deny-list (the advice for uploads holds for reads: a list of
  what is bad is never complete). A folder's own list replaces the policy's for that folder.
- **`anywhere`** is the C programmer's file system: a name may be absolute, and reaches whatever the deny-list and
  the types leave. It exists for a person running their own program on their own machine; it is off unless the
  host says so, and absent unless the build has it (§7). REAPER's JSFX and Kontakt's scripts each began with files
  named only inside a resource folder and each later added absolute paths, which undid the guarantee for every
  script: that is why this one is a decision of the build, not only of a policy.
- **Quotas** are for the program that goes wrong rather than the one that means harm: a `%d` in a loop can name a
  million files.
- **`Program::files()`** lists each use before anything runs: `read presets/*.json`, `list kit/*.f32`,
  `write user/take*.f32`, with its file, line and column. A host can show it, ask, or refuse a program that names a
  folder it does not know.
- **Sandboxed hosts** (an Audio Unit in a sandboxed app, a packaged Windows app, a Flatpak) can give only what
  they have: their container, a folder the user chose. FMinus opens what it is given and asks for nothing more; an
  access that ends shows as `FILE_DENIED`.
- **The driver and executables** have no host, so the person running them says. Neither opens a file by default.

  ```bash
  fminus kit.fm --run load --files-read kit=./samples --files-write user=./takes \
      --files-types f32,json --files-deny ./samples/private
  ```

  An executable reads the same from its environment when it starts: `FMINUS_FILES_READ`, `FMINUS_FILES_WRITE`,
  `FMINUS_FILES_DENY`, `FMINUS_FILES_TYPES` and `FMINUS_FILES_ANYWHERE`.

## 7. Decided when FMinus is built

The request was for a feature configurable at build time, and the answer is the one SQLite gives for its limits: **a
build sets ceilings, and a run can lower them, never raise them.** In SQLite's words, each limit has "a hard upper
bound set at compile-time", and "attempts to increase a limit above its hard upper bound are silently truncated".
FMinus refuses rather than truncates: a policy that asks for more than the build has is an error when the program is
compiled, so a host learns of it at once.

| Option | Default | What a build without it lacks |
|---|---|---|
| `FMINUS_FILES` | `ON` | every `file_` builtin: each is a known name whose call is `'file_read' needs the file interface, which this build of FMinus leaves out (FMINUS_FILES=OFF)`; no code that opens a file is compiled |
| `FMINUS_FILES_WRITE` | `OFF` | `file_write` and `file_append`, the same way; no code that creates, writes or renames a file is compiled, so no policy, option or bug can make such a build write |
| `FMINUS_FILES_ANYWHERE` | `OFF` | absolute names: one is a compile error, and `anywhere` in a policy is refused. Such a build reaches files only through folders a host mapped |
| `FMINUS_FILES_TYPES` | empty: any | every type not in the list (`"wav;f32;json"`): a name with another extension is a compile error, and a policy or a folder that allows one is refused |
| `FMINUS_FILES_MAX_BYTES` | 1 GiB | files larger than it, whatever a policy says |

- **Why reading is on by default and the rest off.** Reading has no dependency to fetch and does nothing until a
  host grants a folder, as the parsers do nothing until handed bytes. Writing changes the user's disk, and names
  outside folders give up the model that makes the rest safe: each is a decision for whoever builds FMinus into
  something, as listening on a socket is.
- **`fminus/Config.h`** has `FMINUS_HAS_FILES`, `FMINUS_HAS_FILES_WRITE`, `FMINUS_HAS_FILES_ANYWHERE`,
  `FMINUS_FILES_TYPES` and `FMINUS_FILES_MAX_BYTES`; a program has `__FMINUS_FILES__` and `__FMINUS_FILES_WRITE__`
  to compile both ways; lit has the features; a `files` preset turns everything on, so one tree tests it all.
- **The compiler uses the ceilings**, which is what build-time buys over a run-time setting: a program that names a
  type the build cannot read, or an absolute path in a build without them, or writes in a build that cannot, is
  refused when it is compiled, at the literal.

## 8. Testing

- **Everything in a directory the test makes** under the build tree, and removes: no test reads or writes outside
  it, and none depends on what the machine has.
- **The traps, each one built for real**: a symbolic link to outside, to inside, as a folder, as the file, in the
  middle of a path; a hard link; a FIFO (which must not hang the test); a directory where a file was named; a file
  that grows while it is read; a file one byte over the limit; a folder renamed after the policy was made; a denied
  directory by another spelling, and one created afterwards; a folder on a case-insensitive file system where the
  machine has one.
- **Every name the compiler must refuse**, with its column, and every name it must not.
- **The three builds**: without files, with reading alone (the default), with everything: each builtin's error or
  effect in each.
- **Wait-free is proved** for `file_status`, `file_size`, `file_data` and `file_count`, as the parsers' reads are
  (`HeapCalls`, and seccomp's strict mode).
- **Writes**: a replace interrupted at each step leaves the old file; quotas; two programs writing one file.
- **Windows and macOS cannot be built where this is written.** Their code is the part of this work most likely to
  be wrong, and the manual must say so until it has run.

## 9. What a host must know

- **Loading belongs to a loader thread**, and FMinus will not let a program forget it.
- **A folder's directory should be one the host owns the meaning of**: its bundle's resources, its own folder in
  the user's library, a directory the user chose. Granting a home directory makes the types and the deny-list do
  all the work.
- **Hidden files are out of reach by construction** (no segment begins with a dot), and so is every name with a
  character Windows would reinterpret.
- **Plugins:** nothing here starts a thread, holds a lock across a call into the host, or keeps a file open after
  a builtin returns, except the folders' own handles, which close with the `Program`.

## 10. What the first version leaves

| Left out | Why | What it would take |
|---|---|---|
| Decoders: WAV, AIFF, FLAC | they turn bytes into numbers, which is the parsers' side of the line, and they are as useful on a download as on a file | a small reader of FMinus's own for WAV and AIFF; a pinned decoder for FLAC |
| Streaming a long file while it plays | it needs a thread and a ring of blocks, and its own design | the earlier report's rings (§4.5 there) |
| Reading part of a file | whole files cover what fits in memory | an offset and a length |
| Removing, renaming, making directories | saving does not need them, and each is a way to lose data | one builtin each, and a policy bit each |
| Following links that stay inside a folder | refusing all of them is the rule that cannot be got wrong | a resolver that re-checks each target, as cap-std has |
| Types by content | an extension says which files are reachable; whether a `.wav` is one is its decoder's to say | a table of magic numbers |
| A file's time, for reloading when it changes | a host knows better when to reload | `file_modified` |
| The Python package | it binds neither the web's options nor these | keyword arguments |

## 11. What was read, and what was not

**Read at its source, 2026-11-11** (appendices A and B have each with where): the system calls and flags of §5 on
Linux (man-pages 6.19, the kernel's headers, glibc's NEWS), macOS (xnu's headers, manual pages and lookup code, by
release) and Windows (Microsoft Learn; Go's `os.Root` as prior art); the security advisories of Wasmtime, cap-std,
Wasmer, WAMR, Deno and Node.js for their file permissions, 2022 to 2026; OWASP on restricting types; how Faust,
JSFX, Kontakt, Cmajor, SuperCollider, Ardour, Csound and Pure Data give scripts files; SQLite's, curl's, Lua's and
others' build-time switches.

**Not verified:**

- **Anything on macOS or Windows by running it.** The macOS facts are from Apple's published source: that
  `O_RESOLVE_BENEATH` is ignored before 15.4 is a reading of that source, not a test. Which Windows build introduced
  `OBJ_DONT_REPARSE` is known only from a comment in Go (build 10240 lacks it).
- How `OBJ_DONT_REPARSE` treats cloud placeholders (OneDrive) and other reparse points that are not links.
- Which directories an out-of-process Audio Unit extension may use: no Apple page says.
- Whether `MoveFileExW` replaces a file in one step on NTFS: Microsoft's page does not promise it.
- Nothing was built or measured for this note: a file's read is the disk's speed, and the reads of what was read
  are two loads.

## Appendix A: what each system gives, and what the authorities say of the audio thread

| Fact | Source |
|---|---|
| `openat2` (Linux 5.6): `RESOLVE_BENEATH` (an escape is `EXDEV`), `RESOLVE_NO_SYMLINKS` (`ELOOP`), `RESOLVE_NO_MAGICLINKS`; unknown flags are `EINVAL`; glibc has a wrapper from 2.43; Docker before 20.10 answers `EPERM` for it | man7.org `openat2(2)`; glibc NEWS; moby's seccomp profiles |
| `OPENAT2_REGULAR` (Linux 7.2): open only a regular file | the kernel's `openat2.h` at v7.2 |
| macOS: `O_NOFOLLOW_ANY` from 11; `O_RESOLVE_BENEATH` from 15.4 only; `O_UNIQUE` (26) refuses a file with several hard links; `F_GETPATH` | xnu's `fcntl.h`, `open.2`, `vfs_lookup.c`, by release tag |
| Windows: `NtCreateFile` relative to a `RootDirectory` handle; `OBJ_DONT_REPARSE` fails at any reparse point "in security scenarios"; `FILE_FLAG_OPEN_REPARSE_POINT` covers the last component only | Microsoft Learn (`NtCreateFile`, `OBJECT_ATTRIBUTES`); Go's `root_windows.go` |
| Windows names: the device names, with extensions and superscript digits; trailing dots and spaces stripped; `name:stream`; `\\?\` turning parsing off; Windows 11 relaxing some of it | Microsoft Learn, "Naming Files, Paths, and Namespaces" and .NET's "File path formats" |
| A FIFO's open blocks without `O_NONBLOCK`; a reported size of 0 for many `/proc` files; a read moves at most 0x7ffff000 bytes | man7.org `fifo(7)`, `inode(7)`, `read(2)` |
| A hard link defeats any policy on paths; Linux's `protected_hardlinks` limits only who may make one | man7.org `proc_sys_fs(5)`; Go's `os.Root` documentation |
| `std::filesystem`: a race is undefined behaviour; `canonical` and `lexically_normal` work on text; `remove_all` had the race of Rust's CVE-2022-21658 in libstdc++ and libc++ | the C++ draft, [fs.race.behavior]; the libraries' fixes of 2022 |
| Replacing a file: temporary file, `fsync`, rename, `fsync` of the directory; macOS's `fsync` does not reach the disk (`F_FULLFSYNC`, `F_BARRIERFSYNC`); Windows's `FileRenameInfoEx` with POSIX semantics, `ReplaceFileW`; Chromium retries the replace on Windows | LWN, "Ensuring data reaches disk"; xnu's `fsync.2`; Microsoft Learn; Chromium's `important_file_writer.cc` |
| Sandboxes: `EACCES` for permissions, `EPERM` for the sandbox and privacy controls; access through a bookmark ends when it is stopped; an Audio Unit lives in its host's sandbox | Apple DTS on the developer forums (2026-09); Apple's App Sandbox documentation; TN2247 |
| "Performing file i/o from an audio callback is a no-go"; "synchronous file I/O ... must be avoided"; "avoid any filesystem access"; the callback "cannot call functions that might block" | Ross Bencina, "Real-time audio programming 101"; Apple QA1715; Steinberg's VST 3 FAQ; JACK's API |
| A mapped file is not safe there: `MAP_POPULATE` only helps, `mlock` is what holds pages | man7.org `mmap(2)`, `mlock(2)` |

## Appendix B: how others gave scripts files, and how it went

**Capabilities, by handle.** WASI has no absolute paths: every path is relative to a directory the host opened, and
a step outside it fails. Its second version dropped the first's per-descriptor rights for three flags. Wasmtime's
own advisories show where such a design still breaks:

| Advisory | Cause |
|---|---|
| cap-std GHSA-hp8f-xmx4-4qrg, Wasmtime CVE-2026-115392 (2026) | the hand-written path walk re-appended a trailing slash, and the kernel followed the link; systems with `openat2` were not affected |
| Wasmtime CVE-2026-47261 (2026) | truncating was not counted as writing: a read-only file could be emptied |
| Wasmtime CVE-2026-58494 (2026) | a hard link or a rename checked only the directory's permission, not the file's |
| Wasmtime CVE-2024-51745 | Windows device names with superscript digits |
| Wasmer CVE-2024-38358 | a link inside a granted directory followed out of it, when creating |
| WAMR CVE-2025-43853 | a link written with a backslash, on Windows |

**Lists of paths, compared as text.** Deno and Node.js check a path's spelling against allowed and denied paths.
Both document that a symbolic link defeats the check, and both have paid for the rest:

| Advisory | Cause |
|---|---|
| Deno CVE-2026-49401 (2026) | macOS: a name in another Unicode normalisation or case passed a deny rule and reached the same file |
| Deno CVE-2025-48888 | an optimisation let an allow rule beat a deny rule |
| Deno CVE-2025-61786, CVE-2024-27931 and others | an operation no one had put the check behind: a stat on an open handle, a temporary file's name |
| Node.js CVE-2025-55130 (2026) | crafted relative links across directories |
| Node.js CVE-2024-21890, CVE-2026-58043, CVE-2026-106071 | prefix and wildcard matching granting a sibling: `/.ssh/*.pub` gave all of `.ssh/` |
| Node.js, fifteen more from 2023 to 2026 | one function at a time found unchecked (`statfs`, `watchFile`, `realpath`, `utimes`, `chmod`), five of them fixes of earlier fixes |

Node's documentation calls its model "a seat belt". The lessons recur: one check behind every operation; decide
from the handle, not the name; refuse what cannot be reasoned about; and when both lists match, deny.

**What compares identities instead.** Linux's Landlock ties each rule to an inode, through a descriptor. POSIX
defines a file's identity as its device and inode, and Windows as its volume's serial number and file id.
Chromium, which must compare text, takes it from the open file (`F_GETPATH`) rather than from the caller, and
refuses UNC paths because one "can be written in a way that can bypass the blocklist".

**Types.** OWASP, on uploads: list the extensions allowed, never those refused; match after decoding the name;
beware the double extension, the trailing dot or space Windows removes, the colon of a stream, letter case; a file's
first bytes "should not be used on its own, as bypassing it is pretty common and easy".

**Audio languages.**

| System | How a script names a file | Confinement |
|---|---|---|
| Faust | `soundfile("label[url:{'a.wav';'b.wav'}]", n)`: literals in the label, a part chosen by an integer; loaded whole when the program starts | the host's list of base directories |
| JSFX (REAPER) | `filename:0,myfile.wav` then `file_open(0)`; or a slider over the files of one data folder | the effect's folder and `Data/`, until absolute paths were added in 6.17 |
| Kontakt's scripts | `load_array`: the file is named by the array, in the instrument's data folder | that folder, until absolute paths were added |
| Cmajor | `external` variables bound to files by the patch's manifest | the patch's bundle |
| SuperCollider's server | paths in commands | `-P`, one restricted directory, checked on text |
| Ardour's Lua | none: `io` and `os` are removed for scripts that process audio | complete |
| Csound, Pure Data | any path | none |

Every idea in this report has a precedent there: naming files up front in the source (Faust, JSFX, Cmajor),
choosing among them by an integer (Faust, JSFX), a resource folder (JSFX, Kontakt, Cmajor), loading whole files
before playing (Faust). So has its main risk.

**Build-time switches.** SQLite: `SQLITE_MAX_*` are ceilings `sqlite3_limit` can lower and not raise, and
`SQLITE_OMIT_LOAD_EXTENSION` removes a mechanism's code altogether, with the warning that such builds are less
tested. libcurl: `CURL_DISABLE_FILE`, and at run time a transfer may use only a subset of what was built. Mbed TLS:
`MBEDTLS_FS_IO`. stb_image and dr_wav: `…_NO_STDIO`. yyjson, which FMinus already builds: `YYJSON_DISABLE_FILE`.
The warning is the lesson: each reduced build needs its own tests.
