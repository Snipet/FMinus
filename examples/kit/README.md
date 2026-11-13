# Kit: a drum kit whose sounds are files

`kit.fm` is a small drum kit ([docs/files.md](../../docs/files.md)). `Kit::load` lists the folder its host maps as
`kit`, reads every raw float table it finds there (`*.f32`, in their names' order, up to eight) into the module's own
arrays, and closes them, off the audio thread; `Kit::process`, `[[clang::nonblocking]]`, plays a pattern of sixteen
steps on those pads and touches no file. In a build with file writing (`FMINUS_FILES_WRITE`), `process` also keeps the
first second it rendered, and `Kit::save` writes it, a take, into the folder `takes`. `kit.cpp` is the host: it grants
the folders, lists the files the program names before running anything, loads, plays and checks. It needs a build with
the file interface, which every build has unless it was configured with `FMINUS_FILES=OFF`; in one without it, it says
so and exits 0. (A build whose `FMINUS_FILES_TYPES` leaves out `f32` or `wav` cannot compile `kit.fm`'s names: it says
that and exits 0 too.)

```bash
build-files/bin/example-kit --check                     # what ctest runs: a folder of its own, under the build tree
build-files/bin/example-kit --write kit                 # the four tables, into ./kit, for the driver (below)
build-files/bin/example-kit --kit kit --takes takes     # play ./kit for a second; save the take into ./takes
```

(`build-files/` is a tree with writing, `cmake --workflow --preset files`; in the default tree, `build-dev/`, the same
runs without the take.)

`--check` makes a folder of its own beside `bin/` (`kit-example/k<pid>/`), writes four drum sounds there as raw
little-endian float32 tables (`CLAP.F32`, `hat.f32`, `kick.f32`, `snare.f32`: made the same way on every machine), and
beside them a table in a subfolder, `private/secret.f32`, and a `notes.wav`. It compiles `kit.fm` with the policy a
host would give it: the folder `kit` with the type `f32` alone and its `private/` on the deny-list, and, with writing,
the folder `takes` granted for writing. It prints what `Program::files()` lists, loads and plays a second in blocks of
480, and compares the samples with the same pattern played in C++ from the tables it wrote: the same, bit for bit. Then:

- `kit/private/secret.f32` is `FILE_REFUSED` (a denied directory), and `kit/notes.wav` is `FILE_REFUSED` (a type the
  folder does not allow); compiled again under a policy with neither rule, both are `FILE_OK`, so it was each rule that
  refused them, not the disk;
- with writing, `takes/take1.f32` holds the samples the program played, byte for byte, and under the policy that grants
  `takes` for reading alone, the same save is `FILE_REFUSED` and makes no file;
- under the default policy, which grants nothing, `Kit::load` is `FILE_REFUSED`, and the kit is silent.

Last, it removes its folder. Nothing outside it is read or written.

```
.../examples/kit/kit.fm:39: list kit/*.f32
.../examples/kit/kit.fm:108: write takes/take*.f32
.../examples/kit/kit.fm:116: read kit/private/secret.f32
.../examples/kit/kit.fm:124: read kit/notes.wav
loaded 4 tables from .../build-files/kit-example/k5345/kit and played 48000 samples, a peak of 1.0302: the same samples as the tables played in C++
kit/private/secret.f32 and kit/notes.wav: FILE_REFUSED with private/ denied and f32 alone allowed, FILE_OK without those rules
saved takes/take1.f32, 192000 bytes, the samples the program played; refused (FILE_REFUSED) where takes is granted for reading alone
checked: the default policy reads nothing (FILE_REFUSED, and silence)
```

The listing's order is the names' bytes, so `CLAP.F32` (an upper-case `C`) is pad 0, then `hat`, `kick` and `snare`;
the pattern `*.f32` finds `CLAP.F32` because a pattern's fixed parts compare without regard to ASCII letter case, on
every system. A hidden file (`.x.f32`), a directory, a link, and a file of another type are never listed.

## By hand, with the driver

The driver has no audio thread, so `kit.fm` has a `main` for it: `main(seconds)` loads the kit, plays that long, and
gives the sum of its samples' magnitudes (which every table changes), or why nothing loaded. With writing,
`bounce(number)` loads, renders a second and saves it as `takes/take<number>.f32`. Run from the repository's root:

```bash
build-files/bin/example-kit --write kit
build-files/bin/fminus examples/kit/kit.fm --run main 1 --files-read ./kit                # 9200.653
build-files/bin/fminus examples/kit/kit.fm --run main 1                                   # -2: FILE_REFUSED, no folder
build-files/bin/fminus examples/kit/kit.fm --run main 1 --files-read ./kit --files-types wav   # -2: f32 not allowed
mkdir takes
build-files/bin/fminus examples/kit/kit.fm --run bounce 1 --files-read ./kit --files-write takes=./takes   # 0
build-files/bin/fminus examples/kit/kit.fm --run bounce 1 --files-read ./kit --files-read takes=./takes    # -2
```

`--files-read ./kit` is the folder `kit`, named after the directory's last segment (`--files-read kit=./samples` names
it outright); `--files-write takes=./takes` grants `takes` for writing too. Each line prints what is in its comment
(run here, in the `files` tree's settings, on Linux). `takes/take1.f32` is then a second of the pattern, 192000 bytes of
float32s: `python3 -c "import array; a = array.array('f'); a.frombytes(open('takes/take1.f32', 'rb').read());
print(len(a), max(map(abs, a)))"` gives `48000 1.0302...`.

As an executable, the same `main`, its folders from the environment ([docs/files.md](../../docs/files.md), "The driver
and executables"):

```bash
build-files/bin/fminus examples/kit/kit.fm --emit exe -o kit-player
FMINUS_FILES_READ=kit=$PWD/kit ./kit-player 1            # 9200.653
./kit-player 1                                           # -2: nothing granted
FMINUS_FILES_READ=kit=$PWD/nowhere ./kit-player 1        # exit status 2: "./kit-player: FMINUS_FILES_READ: folders:
                                                         # 'kit': '.../nowhere' cannot be opened: No such file or directory"
```

## The program

```c
module Kit
{
    float sample[PADS][LENGTH];
    int length[PADS];
    int position[PADS];
    int count;
    int clock;

    int load()
    {
        file_listing kit = file_list("kit/*.f32");
        const int status = file_status(kit);
        count = 0;
        clock = 0;
        if (status == FILE_OK)
        {
            const int n = file_count(kit);
            for (int i = 0; i < n && count < PADS; ++i)
            {
                file_content f = file_read_nth(kit, i);
                if (file_status(f) == FILE_OK)
                {
                    const int values = bytes_floats(file_data(f), file_size(f), sample[count], LENGTH);
                    length[count] = values < LENGTH ? values : LENGTH;
                    position[count] = length[count];
                    count += 1;
                }
                file_close(f);
            }
        }
        file_close(kit);
        return status == FILE_OK ? count : status;
    }

    void process(float* out, int n) [[clang::nonblocking]]
    {
        // ... every pad that is sounding summed, from sample[][]: no file here
    }
};
```

- **The pattern is a literal**, and the folder is the host's. The program says `kit/*.f32`; the host says which
  directory `kit` is on this machine, and the program cannot name another. The host sees the pattern before anything
  runs (`Program::files()`), and grants the folder (`Options::files`): the same program on another machine, or in a
  plugin whose kit is in its bundle, reads its own kit without a change.
- **`load` is off the audio thread.** `file_list`, `file_read_nth` and `file_close` touch the disk and allocate, and in
  a `[[clang::nonblocking]]` function each is an error; `process` reads only the module's arrays. A table is read whole
  into memory before `file_read_nth` returns, never mapped, so even `file_data` and `file_size`, which are wait-free,
  are not needed on the audio thread here: the bytes are copied into `sample` and the file closed.
- **Every failure is a status**, never a trap: `FILE_REFUSED` (no folder `kit` granted, the default), `FILE_MISSING`,
  `FILE_LIMIT` (more tables than the host's `maxListed`). A table that went or grew past the host's limit between the
  listing and its read is skipped.
- **The take** (`#ifdef __FMINUS_FILES_WRITE__`) is written whole or not at all: `file_write` writes a temporary file
  beside the target, flushes it and renames it over the target, so a reader sees the old take or the new one, never
  half.
