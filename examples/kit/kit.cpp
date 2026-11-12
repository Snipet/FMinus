// examples/kit/kit.cpp: the kit example's harness, a host of kit.fm through fminus::Program (docs/files.md). It makes a
// folder of its own under the build tree, writes four drum sounds there as raw float tables (and, beside them, a table
// in a subfolder private/ and a .wav), compiles kit.fm with a policy that maps the folder "kit" to it (f32 alone, and
// private/ denied), lists the files the program names before running anything, loads the kit through Kit::load, plays
// it through Kit::process, and compares the samples with the same pattern played in C++ from the tables it wrote: they
// must be the same, bit for bit. With --check (what ctest runs) it also proves that the default policy reads nothing
// (FILE_REFUSED, and silence), that the denied subfolder stays closed and the .wav is refused (while a policy without
// the deny-list and the type list reads both), and, in a build with file writing, that Kit::save writes the take the
// program rendered into the folder "takes", byte for byte, and is refused where "takes" is granted for reading alone.
// Then it removes what it made. In a build without the file interface it says so and exits 0.
//
//     build-files/bin/example-kit --check                    (its own folder, under the build tree)
//     build-files/bin/example-kit --write DIR                (the four tables, into DIR, for the driver: README.md)
//     build-files/bin/example-kit --kit DIR [--takes DIR]    (play the tables in DIR; save a take into --takes)
#include "fminus/Config.h"
#include "fminus/FMinus.h"

#include <CLI/CLI.hpp>

#include <cstdio>
#include <string>

#if FMINUS_HAS_FILES

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string_view>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#include <malloc.h>    // _aligned_malloc, _aligned_free
#include <process.h>   // _getpid
#else
#include <unistd.h>    // getpid
#endif

namespace fs = std::filesystem;

namespace
{
    // kit.fm's sizes and pattern (its enum Size and its pattern): the C++ reference plays what the program plays.
    constexpr int kPads = 8;
    constexpr int kLength = 12000;
    constexpr int kStep = 6000;
    constexpr int kTake = 48000;
    constexpr int kPattern[16] = {2, 1, 3, 1, 2, 2, 3, 0, 2, 1, 3, 1, 2, 1, 3, 3};

    // The status codes' values (runtime/Files.h): FILE_OK and the two the checks expect.
    constexpr int kFileOk = 0;
    constexpr int kFileRefused = -2;

    // A table: its file's name, and its samples.
    struct Table
    {
        std::string name;
        std::vector<float> samples;
    };

    // Four drum sounds, made the same way on every machine (each sample computed in double and rounded once; the noise
    // from a 32-bit LCG): a kick (a sine falling from 150 Hz to 50 Hz), a snare (a 180 Hz tone and noise), a hat (high
    // noise, short), a clap (three bursts of noise). Their names sort as the listing sorts them, by their bytes:
    // CLAP.F32, hat.f32, kick.f32, snare.f32 (an upper-case name before the lower-case ones; the pattern *.f32 finds
    // CLAP.F32 too, in any letter case).
    std::vector<Table> drumTables()
    {
        constexpr double kPi = 3.14159265358979323846;
        constexpr double kRate = 48000;
        std::uint32_t seed = 0x2545f491u;
        const auto noise = [&] {
            seed = seed * 1664525u + 1013904223u;
            return static_cast<double>(seed >> 8) / 8388608.0 - 1.0;   // [-1, 1)
        };
        std::vector<Table> tables(4);
        tables[0].name = "kick.f32";
        double phase = 0;
        for (int i = 0; i < 9000; ++i)
        {
            const double t = i / kRate;
            phase += 2 * kPi * (50 + 100 * std::exp(-t * 30)) / kRate;
            tables[0].samples.push_back(static_cast<float>(0.9 * std::sin(phase) * std::exp(-t * 8)));
        }
        tables[1].name = "snare.f32";
        for (int i = 0; i < 7000; ++i)
        {
            const double t = i / kRate;
            tables[1].samples.push_back(
                static_cast<float>((0.3 * std::sin(2 * kPi * 180 * t) + 0.5 * noise()) * std::exp(-t * 25)));
        }
        tables[2].name = "hat.f32";
        double last = 0;
        for (int i = 0; i < 2000; ++i)
        {
            const double white = noise();
            tables[2].samples.push_back(static_cast<float>(0.4 * (white - last) * std::exp(-i / kRate * 60)));
            last = white;
        }
        tables[3].name = "CLAP.F32";
        for (int i = 0; i < 6000; ++i)
        {
            const double t = i / kRate;
            const double burst = i < 1500 ? std::exp(-(i % 500) / 80.0) : std::exp(-(t - 0.03) * 20);
            tables[3].samples.push_back(static_cast<float>(0.6 * noise() * burst));
        }
        return tables;
    }

    // Raw little-endian float32s, as bytes_floats reads them, on every machine.
    bool writeFloats(const fs::path& path, const std::vector<float>& samples)
    {
        std::string bytes;
        for (const float sample : samples)
        {
            const auto bits = std::bit_cast<std::uint32_t>(sample);
            for (int shift = 0; shift < 32; shift += 8)
                bytes += static_cast<char>((bits >> shift) & 0xff);
        }
        std::ofstream out(path, std::ios::binary);
        out << bytes;
        return static_cast<bool>(out);
    }

    std::optional<std::string> readBytes(const fs::path& path)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in)
            return std::nullopt;
        return std::string{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    }

    bool writeKit(const fs::path& directory)
    {
        std::error_code error;
        fs::create_directories(directory, error);
        for (const Table& table : drumTables())
        {
            if (!writeFloats(directory / table.name, table.samples))
                return false;
        }
        return true;
    }

    // What kit.fm plays from `tables` (in the listing's order: the names' bytes), in C++: the same steps, the same sums
    // in the same order, so the same floats.
    std::vector<float> reference(std::vector<Table> tables, int samples)
    {
        std::ranges::sort(tables, {}, &Table::name);
        const int count = std::min(static_cast<int>(tables.size()), kPads);
        std::vector<int> position(static_cast<std::size_t>(count));
        std::vector<int> length(static_cast<std::size_t>(count));
        for (int p = 0; p < count; ++p)
        {
            length[p] = std::min(static_cast<int>(tables[p].samples.size()), kLength);
            position[p] = length[p];
        }
        std::vector<float> out(static_cast<std::size_t>(samples));
        for (int clock = 0; clock < samples; ++clock)
        {
            if (count > 0 && clock % kStep == 0)
                position[kPattern[(clock / kStep) % 16] % count] = 0;
            float x = 0.0f;
            for (int p = 0; p < count; ++p)
            {
                if (position[p] < length[p])
                    x += tables[p].samples[static_cast<std::size_t>(position[p]++)];
            }
            out[static_cast<std::size_t>(clock)] = x;
        }
        return out;
    }

    // C's aligned_alloc, and the free that goes with it (Windows: _aligned_malloc and _aligned_free).
    struct Free
    {
        void operator()(void* memory) const
        {
#if defined(_WIN32)
            _aligned_free(memory);
#else
            std::free(memory);
#endif
        }
    };

    // An instance of `module`, as a host makes one: aligned and zeroed.
    std::unique_ptr<void, Free> makeInstance(const fminus::Program::Module& module)
    {
        const std::size_t alignment = std::max(module.alignment, alignof(std::max_align_t));
        const std::size_t size = (module.size + alignment - 1) / alignment * alignment;
#if defined(_WIN32)
        std::unique_ptr<void, Free> instance(_aligned_malloc(size, alignment));
#else
        std::unique_ptr<void, Free> instance(std::aligned_alloc(alignment, size));
#endif
        if (instance)
            std::memset(instance.get(), 0, module.size);
        return instance;
    }

    // kit.fm, compiled under `files`, and its methods (save only in a build with file writing).
    struct Kit
    {
        std::unique_ptr<fminus::Program> program;
        const fminus::Program::Module* module = nullptr;
        int (*load)(void*) = nullptr;
        void (*process)(void*, float*, int) = nullptr;
        int (*save)(void*, int) = nullptr;
        int (*secret)(void*) = nullptr;
        int (*notes)(void*) = nullptr;
    };

    std::optional<Kit> compileKit(const std::string& path, fminus::Program::FilePolicy files, std::string& errors)
    {
        const std::optional<std::string> source = readBytes(path);
        if (!source)
        {
            errors = "cannot read " + path;
            return std::nullopt;
        }
        fminus::Program::Options options;
        options.fileName = path;
        options.files = std::move(files);
        Kit kit;
        kit.program = fminus::Program::compile(*source, errors, options);
        if (!kit.program)
            return std::nullopt;
        fminus::Program& program = *kit.program;
        kit.module = program.module("Kit");
        kit.load = program.function<int(void*)>("Kit::load", errors);
        kit.process = program.function<void(void*, float*, int)>("Kit::process", errors);
        kit.secret = program.function<int(void*)>("Kit::secret", errors);
        kit.notes = program.function<int(void*)>("Kit::notes", errors);
        if (FMINUS_HAS_FILES_WRITE != 0)
            kit.save = program.function<int(void*, int)>("Kit::save", errors);
        if (kit.module == nullptr || kit.load == nullptr || kit.process == nullptr || kit.secret == nullptr
            || kit.notes == nullptr || (FMINUS_HAS_FILES_WRITE != 0 && kit.save == nullptr))
        {
            errors = path + " has no module Kit with its methods: " + errors;
            return std::nullopt;
        }
        return kit;
    }

    // A new kit: loads, plays `samples` samples into `output`, and (when `take` is given, in a build with writing)
    // saves them as takes/take<take>.f32. Kit::load's result, and Kit::save's in `saved`.
    int loadAndPlay(const Kit& kit, std::vector<float>& output, std::optional<int> take = std::nullopt,
                    int* saved = nullptr)
    {
        const auto instance = makeInstance(*kit.module);
        if (!instance)
            return -1000;
        const int loaded = kit.load(instance.get());
        for (std::size_t at = 0; at < output.size(); at += 480)   // blocks of 480, as an audio thread calls it
        {
            const std::size_t block = std::min<std::size_t>(480, output.size() - at);
            kit.process(instance.get(), output.data() + at, static_cast<int>(block));
        }
        if (take && kit.save != nullptr && saved != nullptr)
            *saved = kit.save(instance.get(), *take);
        return loaded;
    }

    // Whether the build's types (FMINUS_FILES_TYPES, empty: any) let the program name its .f32 and .wav files.
    bool buildAllowsTypes()
    {
        const std::string_view types = FMINUS_FILES_TYPES;
        if (types.empty())
            return true;
        bool f32 = false;
        bool wav = false;
        for (std::size_t from = 0; from <= types.size();)
        {
            const std::size_t to = std::min(types.find(';', from), types.size());
            f32 = f32 || types.substr(from, to - from) == "f32";
            wav = wav || types.substr(from, to - from) == "wav";
            from = to + 1;
        }
        return f32 && wav;
    }

    // The directory the checks make their folder in: beside bin/, in the build tree (the executable's own directory's
    // parent: /proc/self/exe on Linux, argv[0] elsewhere), named for this process, so two runs never share one.
    fs::path scratchDirectory(const char* argv0)
    {
        std::error_code error;
        fs::path executable;
#if defined(__linux__)
        executable = fs::read_symlink("/proc/self/exe", error);
#endif
        if (executable.empty() || error)
            executable = fs::absolute(argv0, error);
#if defined(_WIN32)
        const int pid = _getpid();
#else
        const int pid = static_cast<int>(::getpid());
#endif
        return executable.parent_path().parent_path() / "kit-example" / std::format("k{}", pid);
    }

    // Removes the folder the checks made, whatever happens to them.
    struct Removed
    {
        fs::path path;
        ~Removed()
        {
            std::error_code error;
            fs::remove_all(path, error);
            fs::path parent = path.parent_path();
            if (fs::is_empty(parent, error) && !error)
                fs::remove(parent, error);
        }
    };

    int fail(const std::string& message)
    {
        std::fprintf(stderr, "example-kit: %s\n", message.c_str());
        return 1;
    }

    // --check: everything above, in a folder of its own that it removes.
    int check(const std::string& path, const char* argv0)
    {
        const Removed scratch{scratchDirectory(argv0)};
        const fs::path kitDirectory = scratch.path / "kit";
        const fs::path takes = scratch.path / "takes";
        std::error_code made;
        fs::create_directories(kitDirectory / "private", made);
        fs::create_directories(takes, made);
        if (made || !writeKit(kitDirectory) || !writeFloats(kitDirectory / "private" / "secret.f32", {1.0f, 2.0f})
            || !writeFloats(kitDirectory / "notes.wav", {0.5f}))
            return fail("cannot write the kit in " + scratch.path.string());
        std::string errors;

        // The policy a host would give it: the folder "kit", f32 alone, private/ denied; "takes" for writing.
        fminus::Program::FilePolicy policy;
        policy.folders = {{"kit", kitDirectory.string(), false, {"f32"}}};
        policy.deny = {(kitDirectory / "private").string()};
        if (FMINUS_HAS_FILES_WRITE != 0)
            policy.folders.push_back({"takes", takes.string(), true, {}});
        const std::optional<Kit> kit = compileKit(path, policy, errors);
        if (!kit)
            return fail(errors);
        // What a host shows, or checks against what it grants, before it runs anything.
        for (const fminus::Program::FileUse& use : kit->program->files())
            std::printf("%s:%d: %s %s\n", use.file.c_str(), use.line, use.operation.c_str(), use.pattern.c_str());

        std::vector<float> played(kTake);
        int saved = 1;
        const int loaded = loadAndPlay(*kit, played, 1, &saved);
        if (loaded != 4)
            return fail(std::format("Kit::load gave {}, not the kit's 4 tables", loaded));
        if (played != reference(drumTables(), kTake))
            return fail("the kit plays otherwise than its tables played in C++");
        float peak = 0.0f;
        for (const float sample : played)
            peak = std::max(peak, std::fabs(sample));
        std::printf("loaded 4 tables from %s and played %d samples, a peak of %.4f: the same samples as the tables "
                    "played in C++\n",
                    kitDirectory.string().c_str(), kTake, static_cast<double>(peak));

        // The denied subfolder and the type not allowed: refused; and readable under a policy without either rule, so
        // it is the rule that refused them.
        {
            const auto instance = makeInstance(*kit->module);
            if (!instance)
                return fail("out of memory");
            if (const int status = kit->secret(instance.get()); status != kFileRefused)
                return fail(std::format("kit/private/secret.f32, denied: status {}, not FILE_REFUSED (-2)", status));
            if (const int status = kit->notes(instance.get()); status != kFileRefused)
                return fail(std::format("kit/notes.wav, f32 alone allowed: status {}, not FILE_REFUSED (-2)", status));
        }
        fminus::Program::FilePolicy open;
        open.folders = {{"kit", kitDirectory.string(), false, {}}};
        if (FMINUS_HAS_FILES_WRITE != 0)
            open.folders.push_back({"takes", takes.string(), false, {}});   // for reading alone
        const std::optional<Kit> plain = compileKit(path, open, errors);
        if (!plain)
            return fail(errors);
        {
            const auto instance = makeInstance(*plain->module);
            if (!instance)
                return fail("out of memory");
            if (const int status = plain->secret(instance.get()); status != kFileOk)
                return fail(std::format("kit/private/secret.f32, nothing denied: status {}, not FILE_OK", status));
            if (const int status = plain->notes(instance.get()); status != kFileOk)
                return fail(std::format("kit/notes.wav, any type: status {}, not FILE_OK", status));
        }
        std::printf("kit/private/secret.f32 and kit/notes.wav: FILE_REFUSED with private/ denied and f32 alone "
                    "allowed, FILE_OK without those rules\n");

        // The take, in a build with file writing: the program's own samples, on disk; and refused where "takes" is
        // granted for reading alone.
        if (FMINUS_HAS_FILES_WRITE != 0)
        {
            if (saved != kFileOk)
                return fail(std::format("Kit::save gave {}, not FILE_OK", saved));
            const std::optional<std::string> bytes = readBytes(takes / "take1.f32");
            if (!bytes || bytes->size() != played.size() * sizeof(float))
                return fail("takes/take1.f32 is missing, or not the take's size");
            for (std::size_t i = 0; i < played.size(); ++i)
            {
                std::uint32_t bits = 0;
                for (int b = 3; b >= 0; --b)
                    bits = bits << 8 | static_cast<unsigned char>((*bytes)[i * 4 + static_cast<std::size_t>(b)]);
                if (std::bit_cast<float>(bits) != played[i])
                    return fail(std::format("takes/take1.f32's sample {} is not the one the program played", i));
            }
            std::vector<float> again(480);
            int refused = 0;
            loadAndPlay(*plain, again, 2, &refused);
            if (refused != kFileRefused || fs::exists(takes / "take2.f32"))
                return fail(std::format("a save into a folder granted for reading alone: {}, not FILE_REFUSED (-2)",
                                        refused));
            std::printf("saved takes/take1.f32, %zu bytes, the samples the program played; refused (FILE_REFUSED) "
                        "where takes is granted for reading alone\n",
                        bytes->size());
        }

        // The default policy: no files. Nothing loads, and the kit is silent.
        const std::optional<Kit> closed = compileKit(path, {}, errors);
        if (!closed)
            return fail(errors);
        std::vector<float> silence(4800);
        if (const int status = loadAndPlay(*closed, silence); status != kFileRefused)
            return fail(std::format("with no folder granted, Kit::load gave {}, not FILE_REFUSED (-2)", status));
        if (std::ranges::any_of(silence, [](float sample) { return sample != 0.0f; }))
            return fail("with no folder granted, the kit is not silent");
        std::printf("checked: the default policy reads nothing (FILE_REFUSED, and silence)\n");
        return 0;
    }
} // namespace

int main(int argc, char** argv)
{
    CLI::App app{"The kit example: kit.fm lists a folder of raw float tables its host grants, loads them off the audio "
                 "thread and plays them from a nonblocking process",
                 "example-kit"};
    std::string path = std::string(FMINUS_EXAMPLE_DIR) + "/kit.fm";
    std::string write;
    std::string kitDirectory;
    std::string takes;
    bool checking = false;
    app.add_option("--module", path, "The FMinus source of the kit")->capture_default_str();
    app.add_option("--write", write, "Write the kit's four tables into this directory, and do nothing else");
    app.add_option("--kit", kitDirectory, "Play the tables in this directory (the folder kit) for a second");
    app.add_option("--takes", takes, "With --kit: save the second played as take1.f32 in this directory (the folder "
                                     "takes; a build with FMINUS_FILES_WRITE)");
    app.add_flag("--check", checking, "Check only, in a folder of its own under the build tree: the kit plays its "
                                      "files' samples; the default policy, a denied subfolder and a type not allowed "
                                      "read nothing; a take is saved whole");
    CLI11_PARSE(app, argc, argv);

    if (!buildAllowsTypes())
    {
        std::printf("example-kit: this build of FMinus touches only the types %s (FMINUS_FILES_TYPES), and kit.fm "
                    "names .f32 and .wav files: nothing to play\n",
                    FMINUS_FILES_TYPES);
        return 0;
    }
    if (!write.empty())
    {
        if (!writeKit(write))
            return fail("cannot write the tables into " + write);
        std::printf("wrote CLAP.F32, hat.f32, kick.f32 and snare.f32 into %s\n", write.c_str());
        return 0;
    }
    if (checking)
        return check(path, argv[0]);
    if (kitDirectory.empty())
        return fail("nothing to do: --check, --write DIR or --kit DIR (--help)");

    fminus::Program::FilePolicy policy;
    policy.folders = {{"kit", kitDirectory, false, {"f32"}}};
    if (!takes.empty())
    {
        if (FMINUS_HAS_FILES_WRITE == 0)
            return fail("--takes: this build of FMinus leaves out file writing (FMINUS_FILES_WRITE=OFF)");
        policy.folders.push_back({"takes", takes, true, {}});
    }
    std::string errors;
    const std::optional<Kit> kit = compileKit(path, policy, errors);
    if (!kit)
        return fail(errors);
    for (const fminus::Program::FileUse& use : kit->program->files())
        std::printf("%s:%d: %s %s\n", use.file.c_str(), use.line, use.operation.c_str(), use.pattern.c_str());
    std::vector<float> played(kTake);
    int saved = 0;
    const int loaded = loadAndPlay(*kit, played, takes.empty() ? std::nullopt : std::optional<int>(1), &saved);
    if (loaded < 0)
        return fail(std::format("Kit::load: status {}", loaded));
    float peak = 0.0f;
    for (const float sample : played)
        peak = std::max(peak, std::fabs(sample));
    std::printf("loaded %d tables from %s and played %d samples, a peak of %.4f\n", loaded, kitDirectory.c_str(), kTake,
                static_cast<double>(peak));
    if (!takes.empty())
    {
        if (saved != kFileOk)
            return fail(std::format("Kit::save: status {}", saved));
        std::printf("saved take1.f32 into %s\n", takes.c_str());
    }
    return 0;
}

#else

int main(int argc, char** argv)
{
    CLI::App app{"The kit example (a build with the file interface)", "example-kit"};
    bool check = false;
    app.add_flag("--check", check, "Check only");
    app.allow_extras();
    CLI11_PARSE(app, argc, argv);
    std::printf("example-kit: this build of FMinus leaves out the file interface (FMINUS_FILES=OFF): no files to "
                "play\n");
    return 0;
}

#endif
