// examples/wavetable/bench.cpp: the wavetable example's harness. It writes a wavetable pack as a JSON document (four
// tables of one cycle each, three as arrays of numbers and one as base64 of float32s, the gain and the interpolation),
// and the same pack as an XML document (each table a list of numbers, three as an element's text and one in an
// attribute), compiles wavetable.fm through fminus::Program, hands the JSON to Wavetable::init and the XML to
// Wavetable::initXml, as a plugin's preset loader would, plays the tables, and compares the output with C++ playing
// the same floats, and times them. With --check (what ctest runs) it only compares, and fails when the outputs differ
// at all (the XML's from the JSON's too), when calling process a block at a time changes them, when a loader takes a
// document it should refuse (malformed, a table too short, a table with a word in it), or when process is not
// [[clang::nonblocking]]. In a build without one of the parsers (FMINUS_JSON=OFF, FMINUS_XML=OFF), it checks the
// other; without both, it says so and does nothing.
//
//     build-release/bin/example-wavetable               (time in the release preset)
//     build-dev/bin/example-wavetable --block 64
//
// The C++ version is built with -ffp-contract=off (examples/CMakeLists.txt), so that, like FMinus's IR, it never fuses
// a multiply and an add.
#include "fminus/Config.h"
#include "fminus/FMinus.h"

#include <CLI/CLI.hpp>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <malloc.h>   // _aligned_malloc, _aligned_free
#endif

namespace
{
    // C's aligned_alloc, and the free that goes with it (Windows: _aligned_malloc and _aligned_free).
    void* alignedAlloc(std::size_t alignment, std::size_t size)
    {
#if defined(_WIN32)
        return _aligned_malloc(size, alignment);
#else
        return std::aligned_alloc(alignment, size);
#endif
    }

    void alignedFree(void* memory)
    {
#if defined(_WIN32)
        _aligned_free(memory);
#else
        std::free(memory);
#endif
    }

    struct Free
    {
        void operator()(void* memory) const
        {
            alignedFree(memory);
        }
    };

    // The pack: `count` tables of `size` samples, a cycle of a waveform each, as floats.
    struct Pack
    {
        int size = 0;
        float gain = 0.5f;
        bool linear = true;
        std::vector<std::vector<float>> tables;
    };

    // Sine, triangle, saw and square, the last three as the first harmonics of their series (band-limited, as a
    // wavetable's must be), each sample computed in double and rounded once.
    Pack makePack(int size)
    {
        Pack pack;
        pack.size = size;
        constexpr double kPi = 3.14159265358979323846;
        const auto series = [&](int harmonics, auto&& amplitude) {
            std::vector<float> table(static_cast<std::size_t>(size));
            for (int k = 0; k < size; ++k)
            {
                double x = 0;
                for (int h = 1; h <= harmonics; ++h)
                    x += amplitude(h) * std::sin(2 * kPi * h * k / size);
                table[static_cast<std::size_t>(k)] = static_cast<float>(x);
            }
            return table;
        };
        pack.tables.push_back(series(1, [](int) { return 1.0; }));
        pack.tables.push_back(series(31, [&](int h) {   // triangle
            return h % 2 == 0 ? 0.0 : (h % 4 == 1 ? 8 : -8) / (kPi * kPi * h * h);
        }));
        pack.tables.push_back(series(64, [&](int h) { return (h % 2 == 1 ? 2 : -2) / (kPi * h); }));   // saw
        pack.tables.push_back(series(63, [&](int h) { return h % 2 == 0 ? 0.0 : 4 / (kPi * h); }));    // square
        return pack;
    }

    // Bytes as base64 (RFC 4648), with its padding.
    std::string base64(const unsigned char* bytes, std::size_t size)
    {
        constexpr char kDigits[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string text;
        for (std::size_t i = 0; i < size; i += 3)
        {
            const std::uint32_t a = bytes[i];
            const std::uint32_t b = i + 1 < size ? bytes[i + 1] : 0;
            const std::uint32_t c = i + 2 < size ? bytes[i + 2] : 0;
            const std::uint32_t triple = a << 16 | b << 8 | c;
            text += kDigits[triple >> 18 & 63];
            text += kDigits[triple >> 12 & 63];
            text += i + 1 < size ? kDigits[triple >> 6 & 63] : '=';
            text += i + 2 < size ? kDigits[triple & 63] : '=';
        }
        return text;
    }

    // The pack as JSON: each float in the fewest digits that read back as it (std::format's), so the document holds
    // exactly the pack's floats; the third table as base64 of its little-endian float32s.
    std::string toJson(const Pack& pack)
    {
        std::string text = std::format(R"({{"name": "basic shapes", "size": {}, "gain": {}, "interpolation": "{}", )"
                                       R"("tables": [)",
                                       pack.size, pack.gain, pack.linear ? "linear" : "nearest");
        const char* names[] = {"sine", "triangle", "saw", "square"};
        for (std::size_t t = 0; t < pack.tables.size(); ++t)
        {
            text += std::format(R"({}{{"name": "{}", )", t == 0 ? "" : ", ", names[t % 4]);
            const std::vector<float>& table = pack.tables[t];
            if (t == 2)
            {
                std::vector<unsigned char> bytes;
                for (float x : table)
                {
                    const auto bits = std::bit_cast<std::uint32_t>(x);
                    for (int shift = 0; shift < 32; shift += 8)
                        bytes.push_back(static_cast<unsigned char>(bits >> shift));
                }
                text += R"("base64": ")" + base64(bytes.data(), bytes.size()) + "\"}";
                continue;
            }
            text += R"("samples": [)";
            for (std::size_t k = 0; k < table.size(); ++k)
                text += std::format("{}{}", k == 0 ? "" : ", ", table[k]);
            text += "]}";
        }
        return text + "]}";
    }

    // The pack as XML, the same floats written the same way: the third table in its samples attribute, its numbers
    // separated by commas, the others as their element's text, separated by spaces, and lines every eight.
    std::string toXml(const Pack& pack)
    {
        std::string text = std::format("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!-- written by bench.cpp -->\n"
                                       "<pack name=\"basic shapes\" size=\"{}\" gain=\"{}\" interpolation=\"{}\">\n",
                                       pack.size, pack.gain, pack.linear ? "linear" : "nearest");
        const char* names[] = {"sine", "triangle", "saw", "square"};
        for (std::size_t t = 0; t < pack.tables.size(); ++t)
        {
            const std::vector<float>& table = pack.tables[t];
            const bool attribute = t == 2;
            text += std::string("    <table name=\"") + names[t % 4] + (attribute ? "\" samples=\"" : "\">");
            for (std::size_t k = 0; k < table.size(); ++k)
                text += std::format("{}{}", k == 0 ? "" : attribute ? ", " : k % 8 == 0 ? "\n" : " ", table[k]);
            text += attribute ? "\"/>\n" : "</table>\n";
        }
        return text + "</pack>\n";
    }

    // Wavetable::process in C++, line for line, on the pack's floats.
    struct Reference
    {
        const Pack& pack;
        float frequency;
        float position;
        float rate;
        float phase = 0;

        void process(float* output, int n)
        {
            const int count = static_cast<int>(pack.tables.size());
            const int size = pack.size;
            for (int i = 0; i < n; ++i)
            {
                const float where = position * static_cast<float>(count - 1);
                int t = static_cast<int>(where);
                if (t > count - 2)
                    t = count > 1 ? count - 2 : 0;
                const float between = count > 1 ? where - static_cast<float>(t) : 0;
                const int k = static_cast<int>(phase);
                const int next = k + 1 == size ? 0 : k + 1;
                const float within = pack.linear ? phase - static_cast<float>(k) : 0;
                const float* a = pack.tables[static_cast<std::size_t>(t)].data();
                const float* b = count > 1 ? pack.tables[static_cast<std::size_t>(t) + 1].data() : a;
                const float x = a[k] + within * (a[next] - a[k]);
                const float y = b[k] + within * (b[next] - b[k]);
                output[i] = pack.gain * (x + between * (y - x));
                phase += frequency * static_cast<float>(size) / rate;
                if (phase >= static_cast<float>(size))
                    phase -= static_cast<float>(size);
            }
        }
    };

    // wavetable.fm's module, Wavetable: its layout and parameters, and its methods.
    struct Module
    {
        std::unique_ptr<fminus::Program> program;
        const fminus::Program::Module* wavetable = nullptr;
        const fminus::Program::Parameter* frequency = nullptr;
        const fminus::Program::Parameter* position = nullptr;
        bool (*init)(void* wavetable, const void* data, int bytes, float rate) = nullptr;      // JSON
        bool (*initXml)(void* wavetable, const void* data, int bytes, float rate) = nullptr;   // XML
        void (*process)(void* wavetable, float* output, int n) = nullptr;
        void (*end)(void* wavetable) = nullptr;
    };

    bool find(const std::string& path, int level, Module& kernel, std::string& errors)
    {
        std::ifstream file(path);
        if (!file)
        {
            errors = "cannot read " + path;
            return false;
        }
        const std::string source{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
        fminus::Program::Options options;
        options.optimisation = level;
        options.fileName = path;
        kernel.program = fminus::Program::compile(source, errors, options);
        if (!kernel.program)
            return false;
        fminus::Program& program = *kernel.program;
        kernel.wavetable = program.module("Wavetable");
        kernel.frequency = program.parameter("Wavetable::frequency");
        kernel.position = program.parameter("Wavetable::position");
        if (kernel.wavetable == nullptr || kernel.frequency == nullptr || kernel.position == nullptr)
        {
            errors = path + " has no module Wavetable with the parameters frequency and position";
            return false;
        }
        // Each loader is there in a build with its parser.
        if (FMINUS_HAS_JSON != 0)
            kernel.init = program.function<bool(void*, const void*, int, float)>("Wavetable::init", errors);
        if (FMINUS_HAS_XML != 0 && (FMINUS_HAS_JSON == 0 || kernel.init != nullptr))
            kernel.initXml = program.function<bool(void*, const void*, int, float)>("Wavetable::initXml", errors);
        const bool loaders = (FMINUS_HAS_JSON == 0 || kernel.init != nullptr)
                             && (FMINUS_HAS_XML == 0 || kernel.initXml != nullptr);
        kernel.process = loaders ? program.function<void(void*, float*, int)>("Wavetable::process", errors) : nullptr;
        kernel.end = kernel.process ? program.function<void(void*)>("Wavetable::end", errors) : nullptr;
        return kernel.end != nullptr;
    }

    // An instance of Wavetable, as a host makes one: aligned and zeroed, its parameters at their defaults, then set.
    std::unique_ptr<void, Free> makeInstance(const Module& kernel, float frequency, float position)
    {
        const std::size_t alignment = std::max(kernel.wavetable->alignment, alignof(std::max_align_t));
        std::unique_ptr<void, Free> instance(
            alignedAlloc(alignment, (kernel.wavetable->size + alignment - 1) / alignment * alignment));
        if (instance)
        {
            std::memset(instance.get(), 0, kernel.wavetable->size);
            fminus::Program::setDefaults(instance.get(), *kernel.wavetable);
            fminus::Program::setValue(instance.get(), *kernel.frequency, static_cast<double>(frequency));
            fminus::Program::setValue(instance.get(), *kernel.position, static_cast<double>(position));
        }
        return instance;
    }

    // A loader of the module's: init (JSON) or initXml.
    using Init = bool (*)(void*, const void*, int, float);

    // Whether `init` takes the document `text`: an instance made, init run, and end run when it took it.
    bool takes(const Module& kernel, Init init, const std::string& text)
    {
        const std::unique_ptr<void, Free> instance = makeInstance(kernel, 220, 0);
        const bool took = instance && init(instance.get(), text.data(), static_cast<int>(text.size()), 48000);
        if (took)
            kernel.end(instance.get());
        return took;
    }

    // The tables loaded by `init` from `text` and played: one call, or calls of at most `block` samples.
    bool play(const Module& kernel, Init init, const std::string& text, float frequency, float position, float rate,
              std::size_t block, std::vector<float>& output)
    {
        const std::unique_ptr<void, Free> instance = makeInstance(kernel, frequency, position);
        if (!instance || !init(instance.get(), text.data(), static_cast<int>(text.size()), rate))
            return false;
        const std::size_t step = block == 0 ? output.size() : block;
        for (std::size_t start = 0; start < output.size(); start += step)
            kernel.process(instance.get(), output.data() + start,
                           static_cast<int>(std::min(step, output.size() - start)));
        kernel.end(instance.get());
        return true;
    }

    template <typename Run>
    double bestNanoseconds(Run&& run, int repeats)
    {
        double best = std::numeric_limits<double>::infinity();
        for (int r = 0; r < repeats; ++r)
        {
            const auto start = std::chrono::steady_clock::now();
            run();
            const std::chrono::duration<double, std::nano> elapsed = std::chrono::steady_clock::now() - start;
            best = std::min(best, elapsed.count());
        }
        return best;
    }

    int fail(const std::string& message)
    {
        std::fprintf(stderr, "example-wavetable: %s\n", message.c_str());
        return 1;
    }
} // namespace

int main(int argc, char** argv)
{
    CLI::App app{"The wavetable example: wavetable.fm loads a pack from JSON or XML and plays it, against C++, "
                 "compared and timed.",
                 "example-wavetable"};
    std::string path = std::string(FMINUS_EXAMPLE_DIR) + "/wavetable.fm";
    std::size_t samples = 480000;
    std::size_t block = 0;
    int size = 2048;
    int repeats = 20;
    int level = 2;
    float frequency = 110.0f;
    float position = 0.4f;   // between the triangle and the saw
    float rate = 48000.0f;
    bool check = false;
    app.add_option("--module", path, "The FMinus source of the module")->capture_default_str();
    app.add_option("--samples", samples, "Samples per run")
        ->check(CLI::Range(std::size_t{1}, static_cast<std::size_t>(std::numeric_limits<int>::max())))
        ->capture_default_str();
    app.add_option("--block", block, "Samples per call to FMinus (0: one call for the whole run)")
        ->capture_default_str();
    app.add_option("--size", size, "Samples per table")->check(CLI::Range(2, 65536))->capture_default_str();
    app.add_option("--repeat", repeats, "Runs to time (the best counts)")
        ->check(CLI::PositiveNumber)
        ->capture_default_str();
    app.add_option("-O", level, "FMinus's optimisation level, 0 to 3")->check(CLI::Range(0, 3))->capture_default_str();
    app.add_option("--frequency", frequency, "The note, Hz")->check(CLI::Range(1.0f, 20000.0f))->capture_default_str();
    app.add_option("--position", position, "From the first table (0) to the last (1)")
        ->check(CLI::Range(0.0f, 1.0f))
        ->capture_default_str();
    app.add_option("--rate", rate, "The sample rate, Hz")->check(CLI::PositiveNumber)->capture_default_str();
    app.add_flag("--check", check, "Compare only: exit 1 when FMinus and C++ differ at all");
    CLI11_PARSE(app, argc, argv);

    if (FMINUS_HAS_JSON == 0 && FMINUS_HAS_XML == 0)
    {
        std::printf("example-wavetable: this build of FMinus has no JSON or XML parser (FMINUS_JSON=OFF, "
                    "FMINUS_XML=OFF): nothing to run\n");
        return 0;
    }
    std::string errors;
    Module module;
    if (!find(path, level, module, errors))
        return fail("\n" + errors);

    // The loaders this build has, each with the pack written as it reads it.
    struct Loader
    {
        const char* name;
        Init init;
        std::string (*write)(const Pack&);
    };
    std::vector<Loader> loaders;
    if (module.init != nullptr)
        loaders.push_back({"JSON", module.init, toJson});
    if (module.initXml != nullptr)
        loaders.push_back({"XML", module.initXml, toXml});

    const Pack pack = makePack(size);
    std::vector<float> reference(samples);
    Reference cpp{pack, frequency, position, rate};
    cpp.process(reference.data(), static_cast<int>(samples));
    std::vector<std::vector<float>> outputs;
    for (const Loader& loader : loaders)
    {
        outputs.emplace_back(samples);
        if (!play(module, loader.init, loader.write(pack), frequency, position, rate, block, outputs.back()))
            return fail(std::string("Wavetable's loader refused the pack as ") + loader.name);
    }

    if (check)
    {
        bool all = module.program->nonBlocking("Wavetable::process");
        std::printf("example-wavetable: process nonblocking: %s\n", all ? "yes" : "no");
        for (std::size_t l = 0; l < loaders.size(); ++l)
        {
            const Loader& loader = loaders[l];
            const std::string text = loader.write(pack);
            const std::vector<float>& output = outputs[l];
            // Calls of 64 samples, then of 61 (blocks that end mid-way through), must give the output one call does.
            std::vector<float> blocks(samples);
            bool sameInBlocks = true;
            for (const std::size_t calls : {std::size_t{64}, std::size_t{61}})
                sameInBlocks = sameInBlocks && play(module, loader.init, text, frequency, position, rate, calls, blocks)
                               && blocks == output;
            // Nearest samples, not lines between them: the document's choice.
            Pack steps = pack;
            steps.linear = false;
            std::vector<float> nearest(samples);
            std::vector<float> nearestReference(samples);
            Reference stepsCpp{steps, frequency, position, rate};
            stepsCpp.process(nearestReference.data(), static_cast<int>(samples));
            const bool sameNearest =
                play(module, loader.init, loader.write(steps), frequency, position, rate, 0, nearest)
                && nearest == nearestReference && nearest != output;
            // What a loader must refuse: a truncated document, a table a sample short, a word among the samples,
            // nothing, and a pack of no tables.
            const bool json = loader.write == toJson;
            const std::string truncated = text.substr(0, text.size() - (json ? 1 : 3));
            Pack shorter = makePack(size);
            shorter.tables[1].pop_back();
            std::string word = text;
            if (json)
                word.replace(word.find("\"samples\": [") + 12, 1, "\"0\"");
            else
                word.insert(word.find("<table name=\"sine\">") + 19, "zero ");
            const bool refuses = !takes(module, loader.init, truncated)
                                 && !takes(module, loader.init, loader.write(shorter))
                                 && !takes(module, loader.init, word) && !takes(module, loader.init, "")
                                 && !takes(module, loader.init, json ? "{\"tables\": []}" : "<pack size=\"2\"/>");
            const bool same = output == reference && output == outputs.front();
            std::printf("%s", std::format("example-wavetable: a pack of {} tables of {} samples, {} bytes of {}; "
                                          "FMinus and C++ the same over {} samples: {}; in blocks of 64 and 61: {}; "
                                          "nearest samples: {}; broken documents refused: {}\n",
                                          pack.tables.size(), size, text.size(), loader.name, samples,
                                          same ? "yes" : "no", sameInBlocks ? "yes" : "no",
                                          sameNearest ? "yes" : "no", refuses ? "yes" : "no")
                                  .c_str());
            all = all && same && sameInBlocks && sameNearest && refuses;
        }
        return all ? 0 : 1;
    }

    // Loading: init's parse and copy, and end's free, on a fresh instance each time; then playing, which is the same
    // whichever document the tables came from.
    std::printf("%s", std::format("wavetable: {} tables of {} samples, {} Hz at position {}, {} samples at {} Hz\n",
                                  pack.tables.size(), size, frequency, position, samples, rate)
                          .c_str());
    double loadTime = 0;
    for (const Loader& loader : loaders)
    {
        const std::string text = loader.write(pack);
        const double time = bestNanoseconds(
            [&] {
                const std::unique_ptr<void, Free> instance = makeInstance(module, frequency, position);
                loader.init(instance.get(), text.data(), static_cast<int>(text.size()), rate);
                module.end(instance.get());
            },
            repeats);
        loadTime = loadTime == 0 ? time : loadTime;
        std::printf("%s", std::format("  loading {:4} bytes of {:4} (parse, read the tables, free) {:9.1f} us\n",
                                      text.size(), loader.name, time / 1000)
                              .c_str());
    }
    const Loader& first = loaders.front();
    const std::string text = first.write(pack);
    std::vector<float>& output = outputs.front();
    const double fminusTime =
        bestNanoseconds([&] { play(module, first.init, text, frequency, position, rate, block, output); }, repeats);
    const double cppTime = bestNanoseconds(
        [&] {
            Reference again{pack, frequency, position, rate};
            again.process(reference.data(), static_cast<int>(samples));
        },
        repeats);
    const double playTime = (fminusTime - loadTime) / static_cast<double>(samples);
    bool identical = true;
    for (const std::vector<float>& played : outputs)
        identical = identical && played == reference;
    std::printf("%s", std::format("  FMinus, process (-O{})                                       {:9.3f} ns/sample\n"
                                  "  C++, the same loop                                           {:9.3f} ns/sample\n"
                                  "  the outputs are {}\n",
                                  level, playTime, cppTime / static_cast<double>(samples),
                                  identical ? "bit-identical" : "different")
                          .c_str());
    return 0;
}
