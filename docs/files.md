# The file interface

`FMINUS_FILES` gives FMinus programs files: `file_read` reads a whole file into memory and gives a handle,
`file_list` snapshots a folder, `file_read_nth` reads one of the files listed, and in a build with `FMINUS_FILES_WRITE`
`file_write` replaces a file whole and `file_append` adds to one. It is part of the runtime library
(`src/runtime/Files*.cpp`), with no dependency, and on by default; writing, and names outside folders, are off by
default. Through it a program reaches only what its host grants: the default is no files at all, a program names a
file only by a string literal (a folder the host maps, then a path inside it) that the host can list before anything
runs, and what it reaches is decided from what was opened, never from a path's text: each directory is opened beneath
the one before it, no link is ever followed, and each directory passed through is compared by identity with the
denied ones. (The policy is the file builtins', not a sandbox: [what it does not stop](#what-it-does-not-stop) says what
it does not cover.)

```c
module Kit
{
    float sample[8][12000];
    int length[8];
    int count;

    int load()                                               // the host calls it off the audio thread
    {
        file_listing kit = file_list("kit/*.f32");           // a snapshot of the folder "kit", in its names' order
        defer file_close(kit);                               // when load returns
        const int status = file_status(kit);                 // FILE_REFUSED unless the host granted "kit"
        count = 0;
        for (int i = 0; i < file_count(kit) && count < 8; ++i)
        {
            file_content f = file_read_nth(kit, i);          // read whole into memory: refused in a nonblocking function
            defer file_close(f);                             // at the end of each iteration
            if (file_status(f) == FILE_OK)
            {
                const int n = bytes_floats(file_data(f), file_size(f), sample[count], 12000);
                length[count] = n < 12000 ? n : 12000;
                count += 1;
            }
        }
        return status == FILE_OK ? count : status;
    }
};
```

```cpp
fminus::Program::Options options;
options.files.folders = {{"kit", "/Users/me/Music/Kit", false, {"f32"}}};   // nothing mapped: no files
auto program = fminus::Program::compile(source, errors, options);
for (const auto& use : program->files())                   // every name, before anything runs
    show(use.operation, use.pattern, use.line);            // list kit/*.f32 9
```

Contents: [building it](#building-it), [the builtins](#the-builtins), [names, and what the compiler
refuses](#names-and-what-the-compiler-refuses), [the status codes](#the-status-codes), [listings](#listings),
[writing](#writing), [the audio thread](#the-audio-thread), [the host's side](#the-hosts-side), [enforcement: what an
open meets](#enforcement-what-an-open-meets), [what it does not stop](#what-it-does-not-stop), [the driver and
executables](#the-driver-and-executables), [sandboxed hosts and plugins](#sandboxed-hosts-and-plugins), [how it is
built](#how-it-is-built), [what is verified where](#what-is-verified-where), [what is not built](#what-is-not-built).
The research behind it, and the choices it made, is [research/files/README.md](research/files/README.md);
[examples/kit/](../examples/kit/README.md) is a whole program and its host.

## Building it

Reading files is part of every build unless it is configured off; the rest is a decision for whoever builds FMinus
into something. Five options (`cmake/FMinusOptions.cmake`), each a ceiling: a host's policy can narrow what a build
has, never raise it, and a policy that asks for more is refused when the program compiles.

| Option | Default | What a build without it lacks |
|---|---|---|
| `FMINUS_FILES` | `ON` | every `file_` builtin: each is a known name whose call is the error `'file_read' needs the file interface, which this build of FMinus leaves out (FMINUS_FILES=OFF)`, and so is each status code's name; `Options::files` is not read; the driver's `--files-*` options say the build leaves the interface out. None of the file interface's code is compiled: `libfminus_runtime.a` has no `fminus_rt_file` symbol, and its file sources import no `openat` or `fstatat` (the library as a whole still holds pugixml's own `load_file` and `save_file`, through `fopen`, which `FMINUS_XML` compiles in and which no builtin and no function the JIT defines reaches) |
| `FMINUS_FILES_WRITE` | `OFF` (needs `FMINUS_FILES`) | `file_write` and `file_append`, the same way (`'file_write' needs file writing, which this build of FMinus leaves out (FMINUS_FILES_WRITE=OFF)`); a policy folder with `write` is refused, and so are `--files-write` and an executable's `FMINUS_FILES_WRITE`. None of the file interface's code that creates, writes, renames or removes a file is compiled (its sources import no `write`, `renameat` or `unlinkat`), so no policy, option or bug in it can make such a build write (pugixml's unreachable `save_file`, above, aside) |
| `FMINUS_FILES_ANYWHERE` | `OFF` (needs `FMINUS_FILES`) | absolute names: one is a compile error, `anywhere` in a policy is refused, and so are `--files-anywhere` and `FMINUS_FILES_ANYWHERE=1`. Such a build reaches files only through folders a host mapped (`realpath` is not compiled in) |
| `FMINUS_FILES_TYPES` | empty: any | every type not in the list (`"wav;f32;json"`: 1 to 16 ASCII letters and digits each, without the dot; stored in lower case, duplicates dropped): a name with another extension is a compile error, a policy or folder that allows one is refused, and the runtime refuses it again |
| `FMINUS_FILES_MAX_BYTES` | 1 GiB (1073741824) | files larger than it, whatever a policy says: a policy's `maxFileBytes` is at most this (1 to 2147483646) |

Configure stops on a mistake: `FMINUS_FILES_WRITE` or `FMINUS_FILES_ANYWHERE` on with `FMINUS_FILES` off, a type that is
not 1 to 16 letters and digits (`.json`, a 17-letter one), a byte ceiling of 0, over 2147483646 or not a number.
`fminus/Config.h` says `FMINUS_HAS_FILES`, `FMINUS_HAS_FILES_WRITE`, `FMINUS_HAS_FILES_ANYWHERE` (each 0 or 1),
`FMINUS_FILES_TYPES` (a string: `""`, or `"wav;f32"`) and `FMINUS_FILES_MAX_BYTES`.

The `files` preset is `dev` with all three switches on, so one tree tests everything:

```bash
cmake --workflow --preset files            # configure, build and test in build-files/
cmake --preset release -DFMINUS_FILES_WRITE=ON -DFMINUS_FILES_ANYWHERE=ON   # any other tree
cmake --preset dev -DFMINUS_FILES=OFF      # a build with no file interface at all
```

A program compiles both ways with the macros FMinus predefines (to 1) only in a build that has the feature,
`__FMINUS_FILES__` and `__FMINUS_FILES_WRITE__`, which neither a program nor its host may define:

```c
#ifdef __FMINUS_FILES_WRITE__
    int save(int n) { return file_write("takes/take%d.f32", take, 4 * taken, n); }
#endif
```

There is no macro for absolute names or the types: a program that uses either is written for the builds that have
them.

## The builtins

| Builtin | Does | Effect |
|---|---|---|
| `file_content file_read("name", ...)` | opens the file under the host's policy, reads all of it into memory, closes it, and gives a handle | allocates |
| `int file_status(h)` | `FILE_OK`, or why not ([below](#the-status-codes)), of a file or a listing | wait-free |
| `int file_size(f)`, `const void* file_data(f)` | the bytes read and their address, until `file_close`; one 0 byte follows the last, not counted. 0 and null for a file that was not read | wait-free |
| `file_listing file_list("folder/*.ext", ...)` | the regular files of one directory whose names fit the pattern, in the order of their names' bytes, as of now ([listings](#listings)) | allocates |
| `int file_count(list)` | how many | wait-free |
| `string file_name(list, i)` | the `i`-th file's own name (from 0), without its folder (`"pad1.f32"`), a view valid until the listing's `file_close`; empty outside 0 to `file_count - 1`, and for a listing not made | wait-free |
| `file_content file_read_nth(list, i)` | reads the `i`-th (from 0), as `file_read` does | allocates |
| `void file_close(h)` | frees a file's bytes, or a listing | allocates |
| `int file_write("name", data, bytes, ...)` | writes `bytes` bytes at `data` as the whole file, replacing it in one step or not at all ([writing](#writing)); `FMINUS_FILES_WRITE` | allocates |
| `int file_append("name", data, bytes, ...)` | adds them at the file's end, creating it if need be; `FMINUS_FILES_WRITE` | allocates |
| `int file_write("name", json ..., ...)`, `int file_append("name", json ..., ...)` | the same, of a JSON literal's text ([below](#a-json-literal-as-a-file)); `file_append` adds a newline after it (JSON Lines); `FMINUS_FILES_WRITE` and `FMINUS_JSON` | allocates |

- **A name is a string literal**, a format whose `%d`s are the `int`s after every other argument, as a URL's are
  (`file_write("takes/take%d.f32", data, bytes, n)`), checked where it is written ([below](#names-and-what-the-compiler-refuses))
  and recorded for the host: `Program::files()` lists every call, before anything runs.
- **Or it names its values in holes** ([below](#holes)), `file_read("presets/{name}.json")`: each `{name}` a variable
  in scope, an `int`, a `bool`, an enumeration's value or a `string`; the host's list has a `*` for each.
- **A handle is opaque, and knows its kind**: a file, a `file_content`, or a listing, a `file_listing`, C's handles,
  which the compiler keeps apart (`file_count(f)` of a file is an error) while a `void*` converts to either, as in C,
  so a program that says `void*` compiles as before ([extending.md](extending.md), recipe 44). `file_read`, `file_list` and
  `file_read_nth` never return null: a file that could not be read, or a folder that could not be listed, is a handle
  too, one the runtime shares among every failure of its kind and status, which says why (`file_status`), has size 0,
  data null and count 0, and which `file_close` leaves alone. Even when memory runs out the answer is such a handle
  (`FILE_FAILED`).
- **A closed handle reads as no file.** Its memory stays its context's, kept for the next handle the context makes:
  until then its status is `FILE_INVALID`, its size, data and count 0 and null, and closing it again does nothing,
  whatever allocator the host has. Afterwards it is that next handle, as a closed file descriptor's number is a later
  file's: a program that uses a handle after closing it has a bug these rules only keep from being a crash.
- **A wrong handle reads as no file.** A null handle, or one of the other kind that reached the builtin as a `void*`
  (`file_size` of a listing, `file_count` of a file), gives `FILE_INVALID`, 0 or null. `file_read_nth` with an index outside 0 to `file_count - 1`, or of
  something that is not a listing, gives a handle whose status is `FILE_INVALID`. `file_close` of null does nothing.
- **What was read is in memory**, all of it, before the handle is returned: allocated and read into, never mapped. A
  handle's fields never change once it is returned, so the reads are plain loads, and a table may be read straight
  from `file_data` on the audio thread for as long as the handle is open ([the audio thread](#the-audio-thread)).
- **Bytes, not text.** `bytes_floats` and `bytes_pcm16` turn a file's bytes into numbers, and `json_parse` and
  `xml_parse` parse them ([README](../README.md), the builtins), and `string_of(file_data(f), file_size(f))` views them
  as text, a `string` valid until `file_close`.
- **A listing gives its files' names**, `file_name(list, i)`, each the entry's own as the listing holds it (only a name
  a program could have written: [listings](#listings)), valid until the listing is closed. What a name is then good
  for: comparing (`==`, `string_ends`), keeping (`string_copy` into a `char` array), a JSON literal or a reply, and **a
  hole of a later name**, `file_read("kit/{name}")` no (the literal writes its extension), but
  `file_read("kit/{stem}.f32")` with `stem` the name without its extension (`string_slice`), where the run-time
  checks of a string's hole apply as to any string's ([holes](#holes)).
- **The effects** ([extending.md](extending.md), recipe 11): every builtin that touches the disk is in the class that
  allocates, refused in a `[[clang::nonblocking]]` function and in a `[[clang::nonallocating]]` one, directly or
  through a function it calls (with clang's note naming the call); the four reads are wait-free, allowed in both.
  `file_close` frees memory and takes the context's lock, so it allocates in the effects' terms too.

## Names, and what the compiler refuses

```
name    = folder "/" { segment "/" } file          the folder is one the host mapped
        | absolute                                  a build with FMINUS_FILES_ANYWHERE only
folder  = 1 to 64 ASCII letters, digits, "_" and "-", not beginning with "-"
segment = 1 to 255 bytes of UTF-8 (well formed), none of them a control character (U+0000 to U+001F, U+007F to
          U+009F) or one of  \ : * ? " < > |  ; not beginning with "." or a space, not ending with "." or a space;
          not a device name of Windows (CON, PRN, AUX, NUL, COM and LPT with a digit from 0 to 9 or a superscript 1,
          2 or 3, CONIN$, CONOUT$), in any letter case, with or without an extension (spaces before the first "."
          aside: "NUL .wav" is NUL)
file    = a segment with an extension: a "." and 1 to 16 ASCII letters and digits at its end, which the literal
          writes (no %d in it). Only the last extension counts: a.wav.exe is an exe
absolute = "/" { segment "/" } file                on Linux and macOS
         | letter ":/" { segment "/" } file         on Windows
pattern = a name whose file part has one "*", before its extension: "kit/*.f32", "bank%d/pad*.f32"
```

At most **1024 bytes** and **16 segments**, the folder's among them, once the `%d`s are filled in. Every rule is there
because a file system somewhere reads the refused form as something else: `..` and `.` climb or stand still, a leading
`.` hides a file (`.ssh`, `.env` and `.git` are what a careless grant exposes), Windows strips a trailing dot or space,
opens a device for `NUL.txt` and reads `a.wav:x` as a stream of `a.wav`, and `\` is a separator there and a letter
elsewhere. They are refused on every system, so a program means the same on all three. And a name is the entry's own:
FAT and NTFS also find an entry by its 8.3 short name, so `SSH~1` would open `.ssh` and `NOTES~1.WAV` a
`notes.wavx`, past the rules on hidden files and types; the runtime refuses a segment the directory does not list
under that name ([enforcement](#enforcement-what-an-open-meets), step 7).

**Each mistake is an error at its byte**, in clang's manner (`tests/lit/fminus/files-errors.fm` has each one):

| Literal | Error |
|---|---|
| `"kit"`, `""` | `a file's name is a folder its host maps, '/' and a path inside it, as "presets/7.json"` (a listing's says `"kit/*.f32"`) |
| `"kit//a.wav"`, `"kit/"` | `a file's name has no empty segment: no '//', and no '/' at its end` |
| `"k.t/a.wav"`, `"-kit/a.wav"`, `"kit%%/a.wav"` | `character '.' is not allowed in a folder's name: ASCII letters, digits, '_' and '-'`; `a folder's name does not begin with '-'`; the same for `'%'` |
| `"kit/../a.wav"`, `"kit/.hidden.wav"` | `a segment of a file's name does not begin with '.': '.' and '..' and hidden files are out of reach` |
| `"kit/ a.wav"` | `a segment of a file's name does not begin with a space` |
| `"kit/a.wav."`, `"kit/a /b.wav"`, `"kit/sub./a.wav"` | `a segment of a file's name does not end with '.' or a space, which Windows would take away` |
| `"kit/a:b.wav"`, `"kit/a\tb.wav"`, `"kit/a\xc2\x85.wav"` | `character ':' is not allowed in a file's name: no control character, and none of \ : * ? " < > \|` (a control byte as `'\x09'`, U+0085 as `'\xc2\x85'`) |
| `"kit/a\xff.wav"` | `illegal character encoding in a file's name, byte '\xff': a name is UTF-8` |
| `"kit/con.txt"`, `"kit/lpt\xc2\xb9.wav"`, `"kit/sub/aux/x.wav"`, `"kit/nul .wav"` | `'con' is a device name of Windows: no segment of a file's name may be one, in any letter case, with or without an extension` |
| `"kit/COM%d.wav"` | the same, `'COM%d' is a device name of Windows when its %d is a digit: ...` |
| `"kit/readme"`, `"kit/a.wav-x"`, a 17-letter extension | `a file's name ends with its type, an extension: '.' and 1 to 16 ASCII letters and digits, as "7.json"` |
| `"kit/a.%d"`, `"kit/a.w%dv"` | `a file's extension is its type, which the literal writes: no %d in it` |
| seventeen segments | `a file's name has at most 16 segments, the folder's among them` |
| a segment over 255 bytes, a folder over 64 | `a segment of a file's name is at most 255 bytes`; `a folder's name is at most 64 bytes` |
| a name over 1024 bytes, each `%d` one digit | `a file's name is at most 1024 bytes once its %d are filled in` |
| `"kit/%s.wav"` | `invalid conversion specifier 's'`, with the note `a format here takes %d, an int, and %%, a percent sign` |
| `"kit/%d.wav"` with no int; `"kit/a.wav", 3` | `more '%' conversions than data arguments`; `data argument not used by format string` |
| `file_read(3)` | `argument to 'file_read' must be a string literal` |
| `file_list("kit/a.wav")`, `"kit/*a*.wav"` | `a listing's pattern has one '*' in its file part, as "kit/*.f32"`; `a listing's pattern has one '*'` |
| `file_list("kit/a.*")`, `"kit/*"` | `a listing's '*' comes before its extension, which the pattern writes, as "kit/*.f32"` |
| `file_list("kit/*/a.wav")` | `a listing's '*' is in its file part, the name's last segment` |

**The build's ceilings are compile errors too**, at the literal: what deciding them when FMinus is built buys over a
setting at run time.

| Build | Literal | Error |
|---|---|---|
| `FMINUS_FILES_TYPES` `wav;f32` | `"kit/a.ZZZ"` | `'ZZZ' is not a type this build of FMinus can touch (FMINUS_FILES_TYPES: wav;f32)`, at the extension |
| without `FMINUS_FILES_ANYWHERE` | `"/home/me/kick.wav"`, `"C:/Samples/*.wav"` | `an absolute name needs names outside folders, which this build of FMinus leaves out (FMINUS_FILES_ANYWHERE=OFF): a name begins with a folder its host maps, as "presets/7.json"` |
| with it, on Linux or macOS | `"C:/Samples/kick.wav"` | `an absolute name begins with '/', as "/home/me/kick.wav"` (on Windows, `"/x/a.wav"`: `an absolute name on Windows begins with a drive letter, ':' and '/', as "C:/Samples/kick.wav"`) |
| with it | `file_write("/tmp/a.f32", ...)` | `a file is written only in a folder its host grants for writing, never by an absolute name` |
| without `FMINUS_FILES_WRITE` | `file_write(...)`, `file_append(...)` | `'file_write' needs file writing, which this build of FMinus leaves out (FMINUS_FILES_WRITE=OFF)` |

### Holes

A name may name its values where they go instead of `%d`s after it: `file_write("presets/{name}.json", data, bytes)`,
`file_list("kit/{bank}/*.f32")`. A hole is `{identifier}`, a variable in scope where the call is (a local, a parameter,
a field, a constant), of type `int`, `bool` or an enumerated type (written in decimal, as `%d` is) or `string`; not an
expression. A literal has `%d`s or holes, not both, and a literal with holes takes no ints after it. `{{` and `}}` are a
brace, `%%` a percent sign.

- **No hole in the folder** (the first segment, or an absolute name's root) **nor in the extension**, which the
  literal writes: the folder is what the host mapped, the extension the file's type.
- **A `string` joins the segment it is written in**, and cannot add one: the name filled in is checked when it runs by
  the rules the compiler applies to a literal's segments: no `/`, `\` or `*` from the string, no leading `.` or space,
  no trailing `.` or space, none of `: ? " < > |` or a control character, no device name of Windows, UTF-8, 255 bytes a
  segment, 1024 a name, 16 segments. A name that breaks one is **`FILE_INVALID`**, a status the program reads, never a
  trap, and nothing is opened. Then `reach` opens it exactly as any name (the policy's refusals `FILE_REFUSED`).
- **The host's list** (`Program::files()`) has a `*` for each hole, as for each `%d`: `"presets/*.json"`.

Each mistake is an error at its byte, as a literal's are: a brace alone, an empty hole, a name that is none, a hole of
another type (`hole {x} has type 'float': a hole takes an int, a bool, an enumeration's value or a string`), `a file's
folder is its literal's own, a name its host maps: no hole in it`, `a file's extension is its type, which the literal
writes: no hole in it`, a `%d` beside a hole, values after a literal with holes.

The target is the program's (`Options::targetTriple`, or the process's): a name that begins with a drive letter is
judged a Windows name only when the program is compiled for Windows. A name with no `/` at all, such as `"C:\\x"`, gets
the first row's message.

**The runtime checks the name again**, in full, once the `%d`s are filled in (`Context::State::reach`): the compiler's
check is a courtesy, this one the rule. So a name its `%d`s make longer than 1024 bytes (the compiler counted each as
one digit) is `FILE_REFUSED` when it runs, and so is anything a program could not have written, should a name reach the
runtime some other way. A negative `%d` writes its `-`.

## The status codes

`file_status` gives `FILE_OK` or one of these; `file_write` and `file_append` return one. They are names a program
uses without declaring them, `int` constants, as an enumeration's constants are: they work in a `case` label and a
constant expression, and a program's own declaration of such a name hides it (as `HTTP_REFUSED` does:
[web.md](web.md#the-status-codes) says why they are not macros).

| Name | Value | Means |
|---|---|---|
| `FILE_OK` | 0 | done |
| `FILE_MISSING` | -1 | no such file, or no such directory on the way (`ENOENT`); a listed file gone since the listing |
| `FILE_REFUSED` | -2 | the host's policy, or the rules: the program has no files (the default: no folder granted, or no context); a name the rules refuse once filled in; no folder of that name (matched exactly, byte for byte); an absolute name where the build or the policy has none, or written; a folder not granted for writing; a type not allowed; a denied directory or file, however reached; a link anywhere on the way; something that is not a directory where one is walked through, or not a regular file where a file is opened (a FIFO, a device, a socket, a directory); more than one hard link under `singleLink`; on Linux a file on one of the kernel's own file systems (`/proc`, `/sys`, ...); a denied path that cannot be looked up now |
| `FILE_DENIED` | -3 | the operating system said no: `EACCES`, `EPERM`, `EROFS` (on Windows, access denied or a sharing violation): permissions, a sandbox, a read-only volume |
| `FILE_LIMIT` | -4 | over a limit: the file's size (as the system reports it, or as read), the files and listings held (`maxOpen`), the bytes held (`maxHeldBytes`), a listing's length (`maxListed`), the bytes or files a program may write (`maxWrittenBytes`, `maxFilesWritten`), an append past `maxFileBytes` |
| `FILE_FAILED` | -5 | the read or the write itself failed (`EIO`, a full disk, a quota, a file too large for the system), or memory ran out; for `file_write`, also a failure to flush the directory after the rename (the new file is then in place, whole) |
| `FILE_INVALID` | -6 | no file: a null handle, a handle of another kind, an index outside a listing, a negative size or a null `data` with bytes to write |

## Listings

`file_list("kit/*.f32")` reads one directory once: the folder `kit` itself here, or a directory inside it
(`"kit/bank%d/pad*.f32"`). The `*` stands for any bytes within a name (never a `/`); the fixed parts compare without
regard to ASCII letter case, so `*.wav` finds `KICK.WAV` on every system, not only where the file system folds case.

- **A snapshot of names**, taken once: `file_count` and `file_read_nth` mean the same files however the directory
  changes meanwhile. `file_read_nth(list, i)` reads the directory's name plus the `i`-th name as it is now, through
  everything `file_read` checks: a file replaced since is read new, one gone is `FILE_MISSING`, one turned into a link
  `FILE_REFUSED`.
- **In the order of the names' bytes**, FMinus's own (operating systems promise none): `CLAP.F32` before `hat.f32`.
- **Only what a program could have read.** A listing holds regular files whose names the rules allow (no hidden file,
  no device name, nothing with a `:`), of a type allowed in that folder, and, under `singleLink`, with one link. Links,
  directories, FIFOs and anything else are skipped, never listed. A program reads a name by its index, `file_name`, a
  view of the listing's own copy.
- **All or nothing.** More matching files than the policy's `maxListed` is `FILE_LIMIT`, and no listing at all, not
  the first `maxListed`. A listing counts as one of `maxOpen`'s handles.

## Writing

In a build with `FMINUS_FILES_WRITE`, in a folder whose `write` the host set, by a name in that folder (never an
absolute one):

- **`file_write` replaces or does nothing.** It creates a temporary file beside the target, exclusively, through the
  directory's handle (`.fminus-<16 hex digits>.tmp`: hidden, so no program can name it and no listing shows it),
  writes the bytes, flushes them (`fsync`; on macOS `F_BARRIERFSYNC`, falling back to `fsync`), renames it over the
  target (`renameat`, within the directory's handle), and flushes the directory. A reader, or a crash, sees the old file
  or the new one, never half of either. On any failure before the rename the temporary file is removed and the target
  is as it was; a failure to flush the directory after the rename is `FILE_FAILED` with the new file already in place.
- **The target is a regular file or nothing.** A link, a FIFO or a directory at the name is `FILE_REFUSED`, and so is
  a denied file. A link put there between the look and the rename is replaced as a name, never followed.
- **A write whose process dies half way** (killed, a crash, a power cut) leaves its temporary file: the target is
  whole, old or new, but the hidden `.fminus-<16 hex digits>.tmp` stays, and no program can name it. So a context's
  first `file_write` in a directory removes the ones there that have not been written to for an hour (one younger may
  be another process's, still writing): each a regular file with one link and exactly that name's shape, looked at and
  removed by its name beneath the directory's handle, never opened or followed. Run after run, the disk holds at most
  an hour's worth. (On Windows the temporary file is also marked for deletion from when it is made until just before
  the rename, so a dead process's goes with its handle.) A killed write was tried here: 60 writers killed, 39
  temporary files left, all gone at the next write once aged.
- **The new file's mode** is 0666 less the process's umask; the mode of the file it replaces is not kept.
- **`file_append` promises less, and says so.** It opens the file for appending (creating it, exclusively, when it is
  absent), checks that what it opened is the regular file it looked at, with one link (always, `singleLink` or not: a
  hard link would put the bytes in a file elsewhere too), and writes. Nothing is flushed, and a failure half way may
  leave part of the bytes.
- **Sizes.** A write of more than `maxFileBytes`, or an append that would take the file past it, is `FILE_LIMIT`. A
  negative size, or null `data` with bytes to write, is `FILE_INVALID`. A write of 0 bytes makes an empty file.
- **Quotas, for the program that goes wrong** (a `%d` in a loop can name a million files): `maxWrittenBytes` counts
  the bytes of every write and append whose name passed the walk, whether or not they reached the disk, and
  `maxFilesWritten` every `file_write` that got as far as its temporary file and every `file_append` that created its
  file. Neither is ever given back, for the life of the program's context (its `Program`, or an executable's run). A
  write refused before the walk, or over `maxFileBytes`, counts nothing.

### A JSON literal as a file

```c
module Day { int sensor; float mean; float peak; };

Day day[8];
file_write("out/summary.json", json { "days": day });          // {"days":[{"sensor":0,"mean":0.25,"peak":1},...]}
file_write("presets/{name}.json", json (preset));               // a module: an object of its fields
file_append("log/days.jsonl", json { "day": k, "mean": mean });  // one line of JSON Lines, a newline after it
```

`file_write` and `file_append` take a JSON literal in place of their `data` and `bytes`
([web.md](web.md), "The JSON literal", says what a literal holds and how each value is written): the name as any (with
`%d`s after the literal, or holes), and the file's bytes the literal's text, **and, for `file_append`, one newline
after it**, so that appends make JSON Lines (<https://jsonlines.org>: a file a line a value). The text is measured
first (a pass of the runtime's writer that writes nothing), and its size meets every check `bytes` meets, in the same
order: over `maxFileBytes` (the newline counted) is `FILE_LIMIT`, and the quotas count it as they count bytes. Only
then is it written, once, into a buffer of exactly its size, which the file is written from as `data` is: the values
are evaluated once, before either pass. The statuses are the plain form's. It needs the JSON writer too: in a build
without `FMINUS_JSON` the literal is that option's error, and in one without `FMINUS_FILES_WRITE` the call is this
one's. The runtime's functions are `fminus_rt_file_write_json` and `fminus_rt_file_append_json` (`Runtime.h`), which
take the name's strings, none or some, the literal's description and its values' slots.

## The audio thread

Opening, reading, writing and closing a file each wait on the disk for a time no one can bound, and the authorities
the research quotes say the same in the same words: never on the thread that renders. So:

- **Everything that touches the disk allocates**, in the effects' terms, and is refused in `[[clang::nonblocking]]` and
  `[[clang::nonallocating]]` functions, directly or through any function they call. A program loads in `init`, or in a
  method its host calls from a loader thread, and plays on the audio thread from what it loaded.
- **What was read is in memory, all of it, and never mapped.** A mapped file's first touch is a page fault, which is a
  read from the disk on whichever thread touched it: `mmap` would put back on the audio thread the wait the effects keep
  off it. So `file_read` allocates the file's size and reads into it before it returns, and `file_data` and
  `file_size` are plain loads, wait-free, valid until `file_close`.
- **Nothing here is asynchronous.** `http_get` returns at once because a request takes seconds and a thread already
  exists to wait for it. A file takes milliseconds, and FMinus would need a thread of its own to hide them; the host
  has one already, its loader. Streaming a file too long for memory is the case that does need a thread, and it is not
  built ([below](#what-is-not-built)).
- **The reads are proved wait-free**, not asserted: `HeapCalls` counts no heap call in `file_status`, `file_size`,
  `file_data` and `file_count` (on a file, a listing, a failure and null), and a child process under seccomp's strict
  mode, in which any system call but `read`, `write` and `exit` kills it, makes the same calls and exits with the
  expected sums: in the runtime's code (`tests/unit/FilesTest.cpp`) and through a `[[clang::nonblocking]]` function the
  JIT compiled (`tests/unit/FileBuiltinsTest.cpp`), on Linux.
- **LLVM is told so.** A handle's four reads are declared `readonly`, `nofree` and `nosync` (unlike the web library's,
  which another thread changes): two reads of one handle with nothing between are merged, and a `file_read` between
  them is a write to the context's memory, so the read is repeated ([extending.md](extending.md), recipe 43).

**Determinism.** A program that loads, then plays, depends on the files' bytes alone, as it does on a preset its host
hands it: the same files, the same output, bit for bit (`examples/kit/` checks it against C++).

## The host's side

`Program::Options::files` is the policy (`include/fminus/FMinus.h`, `FilePolicy`; in every build, read only in one
with `FMINUS_FILES`):

| Member | Default | |
|---|---|---|
| `folders` | none | the allow-list: each a `name` (what a program's name begins with: 1 to 64 ASCII letters, digits, `_` and `-`, not beginning with `-`; unique, letter case aside; matched exactly), a `directory` (where that is on this machine), `write` (whether `file_write` and `file_append` may work there; `FMINUS_FILES_WRITE`) and `types` (the extensions allowed there, without the dot, any letter case; empty: the policy's). None: no files |
| `deny` | none | directories (or files) no name may lie in, pass through or reach, compared by identity |
| `types` | none | the extensions allowed in a folder with no list of its own; none: any the build allows |
| `anywhere` | false | absolute names, read and listed wherever the deny-list and the types allow, never written (`FMINUS_FILES_ANYWHERE`) |
| `singleLink` | false | refuse a file with more than one hard link |
| `maxFileBytes` | 256 MiB, or the build's ceiling if less | a file read or written, at most (1 to `FMINUS_FILES_MAX_BYTES`) |
| `maxOpen` | 64 | files and listings the program holds at once (1 to 65536) |
| `maxHeldBytes` | 1 GiB | bytes of files read and not yet closed (0 to 2^48) |
| `maxWrittenBytes` | 1 GiB | bytes the program writes in its context's life (0 to 2^48) |
| `maxFilesWritten` | 1024 | files it writes in its context's life (0 to 2^31 - 1) |
| `maxListed` | 4096 | files a listing holds (1 to 1048576): more is `FILE_LIMIT` |

`compile()` checks the policy every time, whether or not the program names a file, and refuses a mistake with the
first one it finds: `fminus::Program: files: folders: 'pre sets' is not a folder's name: 1 to 64 ASCII letters,
digits, '_' and '-', not beginning with '-'`. Besides the ranges above, it refuses a folder named twice (letter case
aside), an empty directory or deny path or one with a NUL byte, a type that is not 1 to 16 letters and digits, and
whatever the build's ceilings do not allow (`folders: 'user': write is set, and this build of FMinus leaves out writing
files (FMINUS_FILES_WRITE=OFF)`; `anywhere is set, and this build of FMinus leaves out absolute names
(FMINUS_FILES_ANYWHERE=OFF)`; `types: 'exe' is not a type this build of FMinus can touch (FMINUS_FILES_TYPES: wav;f32)`;
`maxFileBytes is 2000000000, over this build's ceiling of 1073741824 (FMINUS_FILES_MAX_BYTES)`).

- **The allow-list is a mapping, not a list of paths.** A program says `presets/7.json`; the host says where `presets`
  is on this machine, whether it can be written to and what may be read there. So a program carries no path of the
  machine it was written on, the same source runs in a plugin (the bundle's resources, the user's library) and under
  the driver (a directory on the command line), and a folder the host did not map is refused whatever is on disk.
- **Each folder's directory is opened once**, when the program's context is made, and the grant is that handle, not
  the path it came from: a directory renamed afterwards is still the folder, and one put at the old path is not. Links
  in the host's `directory` text are followed then, once: it is the host's text, not the program's.
- **The deny-list wins**, always, and by identity (device and inode; on Windows the volume's serial number and the
  file's id), never by spelling: `Private`, `private` and a differently normalised `Privé` are one directory on macOS
  and Windows, and so are `kit/private` and `kit/../kit/private` everywhere. A denied directory is closed however a
  name reaches it: through another folder, by a name whose spelling differs, as a folder's own directory or one of its
  ancestors, or a step on the way. Each denied path is held open from when the context is made (so a denied directory
  renamed afterwards is still denied, and its inode cannot be reused while held), and looked up again at every open (so
  one created at the path later is denied too). Whatever an open finds at a denied path is remembered for the context's
  life, so a directory made there afterwards stays denied, once an operation has seen it there, wherever it is renamed
  or moved next. And the paths are looked at again once the walk is done and once the file is read or the directory
  listed: an operation during which what is at a denied path changed is `FILE_REFUSED`, so a directory exchanged in and
  out of a denied path while a name is walked is never passed unseen. What this leaves: a directory made at a denied
  path after the context, and moved away before any operation looked at the path, is not known to be the denied one.
  That takes someone who can write in a granted folder, who could as well copy the files out; deny what exists when
  the program starts. Whatever is put at a denied path is denied from then on, so churn there can only refuse more;
  past 4096 identities remembered, every operation is refused. A deny path that exists but cannot be held fails
  `compile()` (`deny: '<path>' cannot be opened`); one that cannot be looked up at an open for a reason other than its
  absence closes every file (`FILE_REFUSED`): the deny-list fails closed.
- **Types are an allow-list of extensions**, never a deny-list: a list of what is bad is never complete. A folder's own
  list replaces the policy's for that folder; the build's list (`FMINUS_FILES_TYPES`) always applies too. Compared
  without regard to ASCII letter case: `wav` allows `KICK.WAV`.
- **`anywhere`** is the C programmer's file system: a name may be absolute (`"/Users/me/kit/kick.f32"`), and reaches
  whatever the deny-list and the types leave, for reading and listing only. It exists for a person running their own
  program on their own machine; it is off unless the host says so, and absent unless the build has it, because REAPER's
  JSFX and Kontakt's scripts each began with files named only inside a resource folder and each later added absolute
  paths, which undid the guarantee for every script. The links in an absolute name's directory part are followed once,
  to find the real directory (`realpath`), and that real path is then walked from the root under the same rules as a
  program's own name: every segment a name a program could write (so a directory beneath a hidden one, such as
  `~/.cache/x`, stays out of reach), at most 1024 bytes and 16 segments, every directory compared with the denied ones;
  the file itself is never a link. On Windows a UNC path (`\\server\share`) is refused.
- **Quotas and limits** are refusals the program reads, never traps: `FILE_LIMIT`, and the program decides what to do.
  `maxOpen` and `maxHeldBytes` count what the program holds now (a failure's shared handle counts nothing); the write
  quotas count for the context's whole life ([writing](#writing)).
- **`Program::files()`** lists every call of `file_read`, `file_list`, `file_write` and `file_append` in the program,
  before anything runs: its operation (`"read"`, `"list"`, `"write"`, `"append"`), its name as a pattern (each `%d` a
  `*`, each `%%` a `%`; a listing's own `*` stays one), its file, line and column, in the order the calls were lowered.
  Names are literals, so the list is complete: a host can show it, ask its user, or refuse to run a program that names
  a folder it does not know. `file_read_nth` and `file_close` name nothing and are not listed.

**The context.** A program that names a file has a file context: a global of its module, `fminus_rt_files_context`,
which the host's `Program` sets once when it compiles, and which every call that names a file passes to the runtime.
`compile()` makes the context (`fminus::files::Context`, `src/runtime/Files.h`) only when the program names a file and
the policy grants a folder or `anywhere`; otherwise the global stays null, and the runtime refuses every name with no
context (`FILE_REFUSED`), so the default policy costs nothing. A folder whose directory cannot be opened then fails
`compile()`: `fminus::Program: files: folders: 'kit': '/x/samples' cannot be opened: No such file or directory`. The
program cannot name the global, nor the runtime's file functions: any function or constant of a program whose name
begins `fminus_rt_file_` or `fminus_rt_files_` is an error, in every build. Any number of threads may use one context
at once: its quotas and its list of handles take a lock (so `file_read` and `file_close` take it, and the reads do not).

**Lifetime.** `~Program` destroys the context before the JIT goes: every file and listing the program still holds,
closed or not, is freed, and the folders' and the deny-list's handles are closed. A program that never closes what it
reads leaks nothing past its `Program`. No thread is started, no lock is held across a call into the host, and no file
stays open after a builtin returns, except those handles, which close with the `Program`.

## Enforcement: what an open meets

A permission check on a path's text asks the wrong question. What matters is the file the kernel will open for it, and
between a check and an open, that can change. Every operation (`file_read`, `file_list`, `file_read_nth`, `file_write`,
`file_append`) goes through one function, `Context::State::reach` (`src/runtime/Files.cpp`), and then one call of the
system's layer (`src/runtime/FilesSystem.h`). In the order an open meets them:

1. **A context.** None (the default) is `FILE_REFUSED`.
2. **The name, in full**, its `%d`s filled in: the grammar above, strict UTF-8, 1024 bytes, 16 segments, an extension
   the literal wrote. The runtime trusts nothing the compiler checked.
3. **The folder**, by the name's first segment, byte for byte; or, for an absolute name, the build's and the policy's
   `anywhere`, and never for a write. A write or an append needs a folder with `write`.
4. **The type**: the folder's list, else the policy's, and always the build's.
5. **The denied identities as they are now**: those held since the context was made, what is at each denied path now
   (`stat`), and whatever an earlier operation found at one. A path that cannot be looked up, but is not simply absent,
   refuses everything.
6. **The folder's own directory, and its ancestors**, compared with them: the handle's identity, then each directory
   above it, found by opening `..` beneath it one level at a time up to the root (only when the deny-list is not
   empty). So a folder inside a denied directory, or one that was moved into one, is closed.
7. **The walk**: each directory segment of the name opened beneath the last (`openat` with `O_DIRECTORY`, `O_NOFOLLOW`
   and `O_NONBLOCK`; `O_PATH` on Linux, which needs search permission alone), never a separator in what the kernel is
   given, one segment a call; what was opened looked at (`fstat`: a directory, or `FILE_REFUSED`) and compared with the
   denied ones. A link is `ELOOP` (`FILE_REFUSED`); so is anything that is not a directory where one is walked. On a
   file system that may find an entry by a second name (any but Linux's and macOS's own: FAT, NTFS, exFAT, a Windows or
   NFS share, WSL's drives, FUSE), each segment must also be one the directory lists, ASCII letter case aside
   (`nameListed`), before it is used; on Windows, the opened object's long name must be the segment. An absolute name's
   real directory is walked the same way, from `/`. Once the walk is done, the denied paths are looked at again: what
   is at each must be what step 5 found there, or the operation is `FILE_REFUSED`.
8. **The file**, for a read: looked at before it is opened (`fstatat` with `AT_SYMLINK_NOFOLLOW`: only a regular file is
   ever opened, since opening a device runs its driver and opening a FIFO waits for a writer), opened beneath its
   directory (`O_RDONLY`, `O_NONBLOCK`, `O_NOFOLLOW`, `O_NOCTTY`), and looked at again once open (`fstat`: whatever was
   at the name a moment ago, the descriptor is what counts): a regular file, not denied, one link under `singleLink`,
   on Linux not on a kernel file system (`fstatfs`: proc, sysfs, debugfs, tracefs, securityfs, cgroup and cgroup2, bpf,
   configfs, pstore, efivarfs, devpts, binfmt_misc, fusectl, selinuxfs, smackfs, nsfs), and its reported size within
   `maxFileBytes`. It takes one of `maxOpen`'s places first.
9. **The read**, to the limit and one byte more: the reported size is a hint (a file grows while it is read, and some
   report 0). Over the limit is `FILE_LIMIT`, and nothing partial is ever returned. Then the denied paths once more, as
   after the walk, and `maxHeldBytes`.

A listing meets 1 to 7 for its directory, then reads the directory through a descriptor of its own (`openat` of `.`
beneath it) and looks at each name it wants with `fstatat(AT_SYMLINK_NOFOLLOW)`, then the denied paths once more. A
write meets 1 to 7, then [writing](#writing)'s steps, each beneath the directory's handle (the target, when it exists,
a regular file that is not denied, under a name the directory lists where step 7 asks it).

| | Linux | macOS | Windows |
|---|---|---|---|
| Open beneath a directory, no links | `openat` a segment at a time, `O_PATH \| O_DIRECTORY \| O_NOFOLLOW`; `openat2` is not used (the walk must stop at each directory for the deny-list anyway, and a sandbox may refuse the call) | the same walk with `O_RDONLY`, and `O_NOFOLLOW_ANY` (macOS 11) as a second check; `O_RESOLVE_BENEATH` is not relied on | `NtCreateFile` with the directory's handle as `RootDirectory`, `OBJ_DONT_REPARSE` (made without it on a Windows that refuses the flag) and `FILE_OPEN_REPARSE_POINT`; a reparse point, link, junction or mount point, is refused |
| Not a regular file | `fstatat` before, `fstat` after, `S_ISREG`; `O_NONBLOCK` | the same | opened as a file that is not a directory (`FILE_NON_DIRECTORY_FILE`), then looked at: `GetFileType` is `FILE_TYPE_DISK`, not a reparse point, not a device |
| Identity | `st_dev`, `st_ino` | the same | the volume's serial number and the file's 128-bit id (`FILE_ID_INFO`) |
| Ancestors | `..` opened beneath each directory, to the root | the same | each prefix of the handle's final path (`GetFinalPathNameByHandleW`), opened by that path at the moment of the check |
| Kernel file systems | refused (`fstatfs`) | none checked | none checked |
| A second name for an entry | each segment checked against its directory's listing (`readdir`), except on ext2/3/4, XFS, Btrfs, tmpfs, ramfs, overlayfs, ZFS, F2FS, bcachefs, SquashFS and EROFS (`fstatfs`) | the same, except on APFS and HFS+ | the opened object's normalised long name (`GetFinalPathNameByHandleW`) must be the segment, letter case aside (`CompareStringOrdinal`) |
| Replace a file whole | temporary file, `fsync`, `renameat`, `fsync` of the directory | the same, with `F_BARRIERFSYNC` | `NtSetInformationFile` with `FileRenameInformationEx` (POSIX semantics, replacing), else `FileRenameInformation`, retried; the temporary file marked for deletion with its handle from when it is made until just before the rename |
| "Denied" | `EACCES`, `EPERM`, `EROFS` | the same (`EPERM` is also the sandbox and the privacy controls) | access denied, a sharing violation |
| Built and run | here: Linux x86-64 | **never built** | **never built** (`FilesWindows.cpp`) |

`std::filesystem` does none of this work, and none of it uses it: its `canonical` and `lexically_normal` are operations
on text, and the standard calls a race in its functions undefined behaviour.

## What it does not stop

**The policy is the file builtins', not a sandbox**, as [web.md](web.md#the-hosts-side) says of the web library's. FMinus
is C: a prototype with no definition declares an external function, which the JIT resolves in the process, so a
program can declare the C library's own `open` and call it (tried here: `int open(const void* path, int flags);`
compiles, and a call with a name's bytes in an `int` array opens the file, with no policy asked), and a program has
C's pointers. A host has no way yet to refuse a program's external functions. So the policy holds a program that uses
the file builtins to what its host granted, and `Program::files()` tells the host what such a program will ask for;
neither makes it safe to run a program written to get round them. That is FMinus's rule today for every external
function, and closing it is one change to the language's host interface, for the network and files both. Beyond that:

- **A hard link** inside a granted folder to a file elsewhere is read (and listed) unless the host sets `singleLink`;
  then it is `FILE_REFUSED`. A program cannot make one (it has no builtin that links), so someone with write access to
  the folder did. `file_append` refuses one always.
- **A mount** beneath a granted folder is followed: it is a directory there, and what is in it is reachable as the
  rest of the folder is (a denied directory stays denied wherever it is mounted, by its identity).
- **A device swapped in at the last moment.** A file is looked at before it is opened, so a device or a FIFO is never
  opened when it is there at the look; one swapped for a regular file in the instant between the look and the open is
  opened (its driver runs) and then refused by the look that follows. Closing that fully needs Linux 7.2's
  `OPENAT2_REGULAR`, or an `O_PATH` open re-opened through `/proc`.
- **What the host's own text says.** A folder's `directory` and the deny-list's paths are the host's, followed once
  when the context is made (and the deny paths at every open), never a program's.
- **A denied directory nobody saw.** One made at a denied path after the context, and moved away before any operation
  looked at the path, is not known to be the denied one ([the deny-list](#the-hosts-side)): whoever can do that can
  write in a granted folder.
- **The process.** Anything the process can do, the program can, through its own external functions (above).

## The driver and executables

Neither has a host, so the person who runs them says, and by default a program has no files.

- **`fminus --run`** takes `--files-read NAME=DIR` and `--files-write NAME=DIR` (each repeatable; a bare `DIR` is named
  after its last segment, `./samples/` the folder `samples`; a `DIR` that holds `=` is written `NAME=DIR`),
  `--files-deny DIR` (repeatable), `--files-types EXT[,EXT...]` (repeatable) and `--files-anywhere`. The other limits
  are the defaults. Any of them with `--emit` is an error (an executable reads its own at run time), and so are
  `--files-deny` and `--files-types` without a grant to narrow (`--files-deny: needs --files-read, --files-write or
  --files-anywhere, which grant what it narrows`). In a build without the interface, writing or absolute names, the
  option says so (`--files-write: this build of FMinus leaves out file writing (FMINUS_FILES_WRITE=OFF)`). A policy's
  mistake is named by the option it is in (`--files-types: types: '.f32' is not an extension: 1 to 16 ASCII letters and
  digits, without the dot`), and so is a folder that cannot be opened.

  ```bash
  build-files/bin/fminus examples/kit/kit.fm --run main 1 --files-read ./kit --files-types f32
  build-files/bin/fminus examples/kit/kit.fm --run bounce 1 --files-read ./kit --files-write takes=./takes
  ```

- **An executable** (`--emit exe`) of a program that names a file reads its environment when it starts, before the web
  library's: `FMINUS_FILES_READ` and `FMINUS_FILES_WRITE` (`name=directory` entries), `FMINUS_FILES_DENY` (paths),
  each list's entries separated by newlines or the system's separator of a list of paths (`:` on Linux and macOS, `;`
  on Windows: a directory may hold spaces and commas, as in `PATH`, but not that separator); `FMINUS_FILES_TYPES`
  (separated by spaces, commas or semicolons); `FMINUS_FILES_ANYWHERE` (`1`, `0` or empty). A context is made only when
  a folder or `anywhere` is given; a deny-list or types alone make none. A mistake stops it before `main` runs, with
  the variable it is in and why on stderr and exit status 2, as for wrong arguments (`./kit-player: FMINUS_FILES_READ:
  folders: 'kit': '/x/nowhere' cannot be opened: No such file or directory`). Its context is destroyed when `main`
  returns, every file the program holds freed.

  ```bash
  build-files/bin/fminus examples/kit/kit.fm --emit exe -o kit-player
  FMINUS_FILES_READ=kit=$PWD/kit ./kit-player 1
  ```

  In the JIT, `fminus::defineRuntimeSymbols` defines the runtime's file functions by address, from
  `src/runtime/RuntimeSymbols.def`, but never an executable's `fminus_rt_files_main_start` and `_stop`, which read the
  environment.

## Sandboxed hosts and plugins

A sandboxed host (an Audio Unit in a sandboxed app, a packaged Windows app, a Flatpak) can give only what it has: its
container, its bundle's resources, a folder its user chose. FMinus opens what it is given and asks for nothing more. An
access the sandbox refuses, or one that ends (a security-scoped bookmark stopped), shows as `FILE_DENIED`: on macOS,
`EPERM` is the sandbox's and the privacy controls' refusal and `EACCES` the permissions'. Which directories an
out-of-process Audio Unit extension may use, no Apple page says. (All of this is from the research's sources: no
sandboxed host was run.)

- **A folder's directory should be one the host owns the meaning of**: its bundle's resources, its own folder in the
  user's library, a directory the user chose. Granting a home directory makes the types and the deny-list do all the
  work. Hidden files are out of reach by construction (no segment begins with a dot, and none is a short name for one),
  and so is every name with a character Windows would reinterpret.
- **Loading belongs to a loader thread**, and FMinus will not let a program forget it.
- **Unloading.** Nothing here starts a thread; destroy the `Program`s before the plugin's code is unloaded, and the
  folders' handles go with them.

## How it is built

| File | What it is |
|---|---|
| `src/runtime/Files.h` | the host's half, in C++: `fminus::files::Policy`, `checkPolicy`, `Context`, the status codes' values, the name grammar |
| `src/runtime/Files.cpp` | names checked in full, the policy against the build's ceilings, the context and its quotas, the handles, `reach` (every operation's one way in), the C functions, and an executable's `fminus_rt_files_main_start` and `_stop` |
| `src/runtime/FilesSystem.h` | what the runtime asks of the operating system, a directory or a file at a time |
| `src/runtime/FilesPosix.cpp` | Linux and macOS: the walk, the reads, listings, the replace and the append (writing only with `FMINUS_FILES_WRITE`, `realpath` only with `FMINUS_FILES_ANYWHERE`) |
| `src/runtime/FilesWindows.cpp` | Windows, on `NtCreateFile`: never built |
| `src/runtime/Runtime.h`, `RuntimeSymbols.def` | the C functions generated code calls (`fminus_rt_file_*`), which the JIT defines by address; `main_start` and `main_stop` through `FMINUS_FILES_MAIN_SYMBOL`, which a JIT does not define |
| `src/ir/Calls.cpp`, `Lowering.h` | the builtins' table, `Feature::Files` and `Feature::FilesWrite`, `Param::FileName` and `Param::FilePattern`, their effects, and what each runtime function touches (`kRuntimeFacts`) |
| `src/ir/Files.cpp` | a name's checks (`scanFileName`), `files()`'s metadata, the module's context global, the status codes' names |
| `src/ir/Functions.cpp`, `Constants.cpp`, `Memory.cpp` | the reserved names (`fminus_rt_file_`, `fminus_rt_files_`), and the status codes' names in a build without the interface |
| `src/frontend/Preprocessor.cpp` | `__FMINUS_FILES__` and `__FMINUS_FILES_WRITE__` |
| `include/fminus/FMinus.h`, `src/api/FMinus.cpp` | `Options::files`, `FileUse`, `files()`, the `Program`'s context |
| `src/support/Aot.cpp` | an executable's context in its C `main` |
| `tools/fminus/main.cpp` | `--files-read`, `--files-write`, `--files-deny`, `--files-types`, `--files-anywhere` |
| `cmake/FMinusOptions.cmake`, `cmake/templates/Config.h.in` | the five options and their checks, `FMINUS_HAS_FILES*` |
| `tests/unit/FilesTest.cpp` | the runtime from C++ (`[runtime][files]`), every trap built for real in a directory it makes: links (to outside, to inside, as a folder, as the file, mid-path, dangling, looping), hard links, a FIFO, a directory where a file was named, a file that grows while it is read, one byte over the limit, a folder renamed, the deny-list by another spelling, through a link, made afterwards, renamed afterwards, a directory made at a denied path afterwards and renamed or swapped in and out of it while names are walked; a second name a file system answers to (`nameListed`, and every name checked so on this file system); closed handles read and closed again; over 70 refused names and 22 allowed; types; every policy refusal; limits and quotas; listings; writes failed at each step, and the temporary files of writes killed half way, swept; two contexts writing one file; a directory swapped for a link while names are walked; `FILE_DENIED` from a child that gives up root; the executable's environment; the wait-free proof. In a build with `FMINUS_FILES_TYPES`, a case whose files are of a type the build leaves out is skipped |
| `tests/unit/FileBuiltinsTest.cpp` | the builtins in the compiler and the host, in every build: effects, the off build's errors, the errors at their columns, `scanFileName` with the build's ceilings as arguments, `files()`, `Options::files`, the reserved names; then a kit through the JIT and as an executable, absolute names, the wait-free proof through the JIT, and a `Program`'s destructor freeing what its program holds |
| `tests/library/FilesPolicyTest.cpp` | the host API in C++17, in every build |
| `tests/lit/fminus/files*.fm` | the IR and the declarations' attributes, the errors, the driver and executables; `files-off.fm`, `files-write-off.fm`, `files-anywhere-off.fm` and `files-types.fm` in the builds they describe (lit's features `files`, `files-write`, `files-anywhere` and `files-types`) |
| `examples/kit/` | a drum kit that lists and loads its tables, plays them from a nonblocking `process`, and saves a take |

Recipe 43 in [extending.md](extending.md) walks through it as a change to the compiler and the runtime.

## What is verified where

- **Linux x86-64** (clang 18, glibc, a release tree; this machine's kernel 6.18): built with `-Werror` and tested in
  four trees: the default (reading), one with `FMINUS_FILES_WRITE` and `FMINUS_FILES_ANYWHERE` (the `files` preset's
  options), one with `FMINUS_FILES=OFF` (no file symbol in `libfminus_runtime.a`, and every name the off build's
  error), and one with the web library and its server; and every file case of each, with the example, 20 times over
  under `ctest -j4`, each case its own process at once with the others. A fifth tree with `FMINUS_FILES_TYPES=wav;F32`
  passed its whole suite, the cases whose files are of other types skipped. Every test, lit test and the example works
  in a directory it made (beside `bin/`, or lit's `%t`) and removes it; `/dev/null`, `/proc/self/status` and
  `/proc/config.gz` are named only as what must be refused. Scratch configures checked each of configure's refusals.
- **Outside the tree**, by hand, as the web library's were: the runtime's sources and `FilesTest.cpp` under GCC 13's
  AddressSanitizer with UndefinedBehaviorSanitizer (all 28 cases: "All tests passed (1378 assertions in 28 test
  cases)") and ThreadSanitizer (26, without the seccomp case and the `FILE_DENIED` one, whose forked children cannot
  run under ThreadSanitizer's background thread); a race harness that exchanged a folder's subdirectory and a file with
  links to outside (`renameat2` with `RENAME_EXCHANGE`) while three threads read, listed and read the listed files (40
  seconds under AddressSanitizer, 4.0 million swaps; 30 under ThreadSanitizer, 3.1 million), with no byte from
  outside returned; a second one that also exchanged a denied directory, one pinned and one made after the context,
  with public ones, while writes, appends and reads ran (2.0 and 1.5 million swaps): nothing written outside, nothing
  denied read; a fuzzer that damaged 300,000 valid names (`../`, `\`, `:`, NUL, bad UTF-8, device names, `%d`s,
  truncations) and read and listed each, which returned nothing from outside the folder, nothing hidden and nothing
  denied; 60 writers killed half way, whose 39 temporary files the next write swept once aged; and a handle closed
  twice, clean under AddressSanitizer. `FilesWindows.cpp` and `Files.cpp`'s Windows lines were type-checked on Linux
  (`clang++ -fsyntax-only -D_WIN32`, with the build's warnings and `-Werror`) against stub headers written from the
  Windows SDK's declarations: that finds a wrong type or name, not a wrong behaviour.
- **Holes and `file_name`** (2026-11-13): built and tested in the default tree and in one with the web library, its
  server, writing and absolute names, not in one with `FMINUS_FILES=OFF` nor with `FMINUS_FILES_TYPES` set; the
  runtime's half (`fminus_rt_file_read_s` and the rest, `fminus_rt_file_name`) ran under the sanitizers when it was
  written; through the JIT, a name a string makes wrong is `FILE_INVALID` (`tests/unit/HoleTest.cpp`), and a
  nonblocking function reading a listing's names allocates nothing and makes no system call
  (`tests/unit/DocumentTextTest.cpp`). `examples/presets/` writes and reads its files by names with holes, in a
  directory its check makes.
- **A JSON literal as a file** (2026-11-13): built and tested in the default tree (whose build has no writing: the
  option's error) and in one with the web library, its server, writing and absolute names, not in one without
  `FMINUS_JSON` (whose errors are written and not run), nor with `FMINUS_FILES=OFF` or `FMINUS_FILES_TYPES` set.
  `fminus_rt_file_write_json` and `_append_json` from C++ (`tests/unit/JsonFileTest.cpp`: the text, JSON Lines, the
  size limit with the newline, the quota, the refusals, a description the writer refuses), and through the JIT every
  kind of value written and read back (`tests/unit/JsonLiteralTest.cpp`), as an executable (`json-literal.fm`); the
  runtime's sources and those tests under GCC 13's AddressSanitizer with UndefinedBehaviorSanitizer and under
  ThreadSanitizer ([extending.md](extending.md), recipe 51). `examples/presets/` stores each preset as `json (preset)`.
- **Not run anywhere yet:** macOS (the `O_RDONLY` walk, `O_NOFOLLOW_ANY`, `F_BARRIERFSYNC`, `EMLINK`'s meaning,
  `fstatfs`'s type names, the tests' non-Linux paths) and Windows (all of `FilesWindows.cpp`: `NtCreateFile` with and
  without `OBJ_DONT_REPARSE`, `FILE_ID_INFO`, ancestors by the final path, the long-name check, listing by
  `FileIdBothDirectoryInfo`, the rename classes 65 and 10 and their retries, the temporary file's delete disposition
  and the sweep, drive paths for `anywhere`; the driver's `\` in a bare `DIR`; a Windows target's absolute names
  through the compiler, tested only by giving `scanFileName` Windows's rule; an executable's start and stop there);
  arm64; the `files` preset itself (a `dev` tree: this container builds only the `release` flavour) and the sanitizer
  presets; a sanitizer run of the compiler's and the host's part; a FAT, NTFS, exFAT, SMB, NFS, 9p or FUSE volume,
  where the check against short names matters (it ran here on every name of every file case's kind through a test
  hook, and its comparison alone against a listing of `.ssh`, but no file system here answers to a second name); a
  case-insensitive or normalising file system; `FILE_DENIED` from a sandbox, as opposed to permissions; and
  out-of-memory paths, `EINTR`, the 4096-level limit on ancestors and on remembered identities, a temporary name's
  collision, an append whose name changes twice, and a file swapped for a device between the look and the open.

## What is not built

The research's §10, as it stands:

| Left out | Why | What it would take |
|---|---|---|
| Decoders: WAV, AIFF, FLAC | they turn bytes into numbers, which is the parsers' side of the line, and they are as useful on a download as on a file | a small reader of FMinus's own for WAV and AIFF; a pinned decoder for FLAC |
| Streaming a long file while it plays | it needs a thread and a ring of blocks, and its own design | the earlier report's rings |
| Reading part of a file | whole files cover what fits in memory | an offset and a length |
| Removing, renaming, making directories | saving does not need them, and each is a way to lose data | one builtin each, and a policy bit each |
| Following links that stay inside a folder | refusing all of them is the rule that cannot be got wrong | a resolver that re-checks each target, as cap-std has |
| Types by content | an extension says which files are reachable; whether a `.wav` is one is its decoder's to say | a table of magic numbers |
| A file's time, for reloading when it changes | a host knows better when to reload | `file_modified` |
| The Python package | it binds neither the web's options nor these ([python.md](python.md)) | keyword arguments |
