// tests/library/Array2DTest.cpp: 2-D arrays as a host meets them (C++17, FMinus::FMinus alone): a module's 2-D fields
// as module() describes them (the type "float[4][16]", the element type and the extents, outermost first, at an offset,
// row by row; a 1-D field's and an array of modules' too), which the host reads and writes as C++'s float[4][16]; a
// module shared by several (one bank of wavetables that every voice reads, as a host's shared state is); and 2-D arrays
// in a program compiled with the loop budget, with bounds checks (each dimension checked, the message naming which:
// 'gain' or 'gain[v]') and around #if, as the safety options and the conditionals meet them.
#include "fminus/FMinus.h"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <malloc.h>   // _aligned_malloc, _aligned_free
#endif

namespace
{
    // An instance of a module, as a host makes one: aligned, zeroed, the module's size (FMinus.h, Module).
    struct Instance
    {
        explicit Instance(const fminus::Program::Module& module)
        {
            const std::size_t alignment = module.alignment < alignof(std::max_align_t) ? alignof(std::max_align_t)
                                                                                       : module.alignment;
            const std::size_t size = (module.size + alignment - 1) / alignment * alignment;
#if defined(_WIN32)
            memory = _aligned_malloc(size, alignment);
#else
            memory = std::aligned_alloc(alignment, size);
#endif
            std::memset(memory, 0, size);
        }
        ~Instance()
        {
#if defined(_WIN32)
            _aligned_free(memory);
#else
            std::free(memory);
#endif
        }
        Instance(const Instance&) = delete;
        Instance& operator=(const Instance&) = delete;

        void* memory = nullptr;
    };

    // A field's description, by name.
    const fminus::Program::Module::Field* fieldNamed(const fminus::Program::Module& module, const std::string& name)
    {
        for (const fminus::Program::Module::Field& field : module.fields)
        {
            if (field.name == name)
                return &field;
        }
        return nullptr;
    }

    // A bank of wavetables, shared: four tables of 64 samples, filled once by init, read by every voice through a
    // pointer to the bank; and a voice that reads one table, a row, as a const float*.
    const char* const kBank = "enum { TABLES = 4, SIZE = 64 };\n"
                              "module Shared\n"
                              "{\n"
                              "    float tables[TABLES][SIZE];\n"
                              "    int counts[2][3];\n"
                              "    double slow[2][2];\n"
                              "    float level;\n"
                              "\n"
                              "    void init()\n"
                              "    {\n"
                              "        for (int t = 0; t < TABLES; ++t)\n"
                              "            for (int i = 0; i < SIZE; ++i)\n"
                              "                tables[t][i] = t + i / 64.0f;\n"
                              "    }\n"
                              "};\n"
                              "float lookup(const float* table, float phase) { return table[(int)(phase * SIZE) % SIZE];"
                              " }\n"
                              "module Voice\n"
                              "{\n"
                              "    Shared* shared;\n"
                              "    int table;\n"
                              "    float phase;\n"
                              "\n"
                              "    float tick(float step)\n"
                              "    {\n"
                              "        phase += step;\n"
                              "        if (phase >= 1)\n"
                              "            phase -= 1;\n"
                              "        return lookup(shared->tables[table], phase);\n"
                              "    }\n"
                              "};\n"
                              "module Choir\n"
                              "{\n"
                              "    float taps[16];\n"
                              "    Voice voices[2][3];\n"
                              "    Voice solo[2];\n"
                              "};\n";
} // namespace

TEST_CASE("a module's 2-D fields are described by their shape, and read and written as C++'s, row by row",
          "[library][array2d]")
{
    std::string errors;
    auto program = fminus::Program::compile(kBank, errors);
    INFO(errors);
    REQUIRE(program);
    const fminus::Program::Module* shared = program->module("Shared");
    REQUIRE(shared != nullptr);
    const auto* tables = fieldNamed(*shared, "tables");
    const auto* counts = fieldNamed(*shared, "counts");
    const auto* slow = fieldNamed(*shared, "slow");
    const auto* level = fieldNamed(*shared, "level");
    REQUIRE(tables != nullptr);
    REQUIRE(counts != nullptr);
    REQUIRE(slow != nullptr);
    REQUIRE(level != nullptr);
    CHECK(tables->type == "float[4][64]");
    CHECK(tables->element == "float");
    CHECK(tables->extents == std::vector<std::size_t>{4, 64});
    CHECK(tables->offset == 0);
    CHECK(tables->size == 4 * 64 * sizeof(float));
    CHECK(counts->type == "int[2][3]");
    CHECK(counts->extents == std::vector<std::size_t>{2, 3});
    CHECK(counts->offset == 1024);
    CHECK(counts->size == 24);
    CHECK(slow->type == "double[2][2]");
    CHECK(slow->element == "double");
    CHECK(slow->offset == 1048);   // aligned to 8
    CHECK(slow->size == 32);
    CHECK(level->type == "float");
    CHECK(level->element.empty());
    CHECK(level->extents.empty());
    const fminus::Program::Module* voice = program->module("Voice");
    REQUIRE(voice != nullptr);
    const auto* pointer = fieldNamed(*voice, "shared");
    REQUIRE(pointer != nullptr);
    CHECK(pointer->type == "Shared *");
    CHECK(pointer->element.empty());
    CHECK(pointer->extents.empty());
    // A 1-D field's shape, and arrays of modules: their element the module's name.
    const fminus::Program::Module* choir = program->module("Choir");
    REQUIRE(choir != nullptr);
    const auto* taps = fieldNamed(*choir, "taps");
    const auto* voices = fieldNamed(*choir, "voices");
    const auto* solo = fieldNamed(*choir, "solo");
    REQUIRE(taps != nullptr);
    REQUIRE(voices != nullptr);
    REQUIRE(solo != nullptr);
    CHECK(taps->type == "float[16]");
    CHECK(taps->element == "float");
    CHECK(taps->extents == std::vector<std::size_t>{16});
    CHECK(taps->size == 16 * sizeof(float));
    CHECK(voices->type == "Voice[2][3]");
    CHECK(voices->element == "Voice");
    CHECK(voices->extents == std::vector<std::size_t>{2, 3});
    CHECK(voices->offset == 64);
    CHECK(voices->size == 6 * voice->size);
    CHECK(solo->type == "Voice[2]");
    CHECK(solo->element == "Voice");
    CHECK(solo->extents == std::vector<std::size_t>{2});
    CHECK(solo->offset == voices->offset + voices->size);

    Instance bank(*shared);
    auto* init = program->function<void(void*)>("Shared::init", errors);
    REQUIRE(init != nullptr);
    init(bank.memory);
    // The host's view: C++'s float[4][64] at the field's offset.
    float (*rows)[64] = reinterpret_cast<float (*)[64]>(static_cast<char*>(bank.memory) + tables->offset);
    CHECK(rows[0][0] == 0.0f);
    CHECK(rows[2][32] == 2.5f);
    CHECK(rows[3][63] == 3.0f + 63 / 64.0f);
    rows[1][16] = 100.0f;   // the host writes one, and a voice reads it

    // Two voices sharing the bank, each reading its own table (a row, as a const float*).
    Instance first(*voice);
    Instance second(*voice);
    auto* tick = program->function<float(void*, float)>("Voice::tick", errors);
    REQUIRE(tick != nullptr);
    const auto set = [&](Instance& instance, const char* name, const void* value, std::size_t size) {
        const auto* field = fieldNamed(*voice, name);
        REQUIRE(field != nullptr);
        std::memcpy(static_cast<char*>(instance.memory) + field->offset, value, size);
    };
    void* address = bank.memory;
    const int one = 1;
    const int three = 3;
    set(first, "shared", &address, sizeof address);
    set(first, "table", &one, sizeof one);
    set(second, "shared", &address, sizeof address);
    set(second, "table", &three, sizeof three);
    CHECK(tick(first.memory, 0.25f) == 100.0f);   // tables[1][16]: the host's write
    CHECK(tick(second.memory, 0.5f) == 3.5f);     // tables[3][32]
}

TEST_CASE("2-D arrays meet the loop budget, bounds checks and the conditionals", "[library][array2d]")
{
    fminus::Program::Options options;
    options.loopBudget = true;
    options.boundsCheck = true;
    options.defines["ROWS"] = "4";
    const char* const source = "#if ROWS > 2\n"
                               "module Bank\n"
                               "{\n"
                               "    float gain[4][16];\n"
                               "    float sum(int v) { float s = 0; for (int i = 0; i < 16; ++i) s += gain[v][i];"
                               " return s; }\n"
                               "    float at(int v, int i) { return gain[v][i]; }\n"
                               "    float spin(float x) { while (x > 0) x = x * gain[0][1] + 1; return x; }\n"
                               "};\n"
                               "#else\n"
                               "module Bank { float gain[16]; };\n"
                               "#endif\n";
    std::string errors;
    auto program = fminus::Program::compile(source, errors, options);
    INFO(errors);
    REQUIRE(program);
    const fminus::Program::Module* module = program->module("Bank");
    REQUIRE(module != nullptr);
    REQUIRE(module->fields.size() == 1);
    CHECK(module->fields[0].type == "float[4][16]");
    CHECK(module->fields[0].extents == std::vector<std::size_t>{4, 16});

    Instance bank(*module);
    float (*gain)[16] = static_cast<float (*)[16]>(bank.memory);
    gain[2][15] = 1.5f;
    auto* sum = program->function<float(void*, int)>("Bank::sum", errors);
    auto* at = program->function<float(void*, int, int)>("Bank::at", errors);
    auto* spin = program->function<float(void*, float)>("Bank::spin", errors);
    REQUIRE(sum != nullptr);
    REQUIRE(at != nullptr);
    REQUIRE(spin != nullptr);
    program->setLoopBudget(1000);
    CHECK(sum(bank.memory, 2) == 1.5f);
    CHECK_FALSE(program->loopBudgetExceeded());
    spin(bank.memory, 1.0f);   // for ever, gain[0][1] being 0, but for the budget
    CHECK(program->loopBudgetExceeded());

    program->setLoopBudget(1000);
    CHECK(at(bank.memory, 2, 15) == 1.5f);
    CHECK_FALSE(program->boundsViolation().has_value());
    at(bank.memory, 1, 16);   // the next row's first float in memory, but no element of row 1
    std::optional<fminus::Program::BoundsViolation> violation = program->boundsViolation();
    REQUIRE(violation.has_value());
    CHECK(std::string(violation->array) == "gain[v]");
    CHECK(std::string(violation->type) == "float[16]");
    CHECK(violation->extent == 16);
    CHECK(violation->index == 16);
    CHECK(violation->message() == "program.fm:6:44: index 16 out of bounds for 'gain[v]' of type 'float[16]'");
    program->clearBoundsViolation();
    at(bank.memory, 4, 0);
    violation = program->boundsViolation();
    REQUIRE(violation.has_value());
    CHECK(std::string(violation->array) == "gain");
    CHECK(std::string(violation->type) == "float[4][16]");
    CHECK(violation->extent == 4);
    CHECK(violation->message() == "program.fm:6:41: index 4 out of bounds for 'gain' of type 'float[4][16]'");
    program->clearBoundsViolation();
}
