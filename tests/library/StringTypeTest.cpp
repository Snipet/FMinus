// tests/library/StringTypeTest.cpp: the type string and char arrays as a host meets them (C++17, FMinus::FMinus alone):
// a module's string field, its array of strings and its char array as module() describes them, a string 16 bytes
// aligned to 8 (a pointer and an int, as C's struct { const char* data; int size; } is laid out); and a function that
// takes or returns a string, which call() refuses, saying why, as it refuses a vector.
#include "fminus/FMinus.h"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <string>
#include <vector>

namespace
{
    const fminus::Program::Module::Field* fieldNamed(const fminus::Program::Module& module, const std::string& name)
    {
        for (const fminus::Program::Module::Field& field : module.fields)
        {
            if (field.name == name)
                return &field;
        }
        return nullptr;
    }
} // namespace

TEST_CASE("a string field is described by its type's name, 16 bytes aligned to 8; a char array as an array",
          "[library][string-type]")
{
    std::string errors;
    auto program = fminus::Program::compile("module Preset\n"
                                            "{\n"
                                            "    int count;\n"
                                            "    string label;\n"
                                            "    string names[3];\n"
                                            "    char text[2][5];\n"
                                            "    float gain;\n"
                                            "};\n",
                                            errors);
    INFO(errors);
    REQUIRE(program);
    const fminus::Program::Module* preset = program->module("Preset");
    REQUIRE(preset != nullptr);
    const auto* label = fieldNamed(*preset, "label");
    const auto* names = fieldNamed(*preset, "names");
    const auto* text = fieldNamed(*preset, "text");
    const auto* gain = fieldNamed(*preset, "gain");
    REQUIRE(label != nullptr);
    REQUIRE(names != nullptr);
    REQUIRE(text != nullptr);
    REQUIRE(gain != nullptr);
    CHECK(label->type == "string");
    CHECK(label->offset == 8);   // after the int, at the pointer's alignment
    CHECK(label->size == 16);
    CHECK(label->element.empty());
    CHECK(names->type == "string[3]");
    CHECK(names->element == "string");
    CHECK(names->extents == std::vector<std::size_t>{3});
    CHECK(names->offset == 24);
    CHECK(names->size == 48);
    CHECK(text->type == "char[2][5]");
    CHECK(text->element == "char");
    CHECK(text->extents == std::vector<std::size_t>{2, 5});
    CHECK(text->offset == 72);
    CHECK(text->size == 10);
    CHECK(gain->offset == 84);
    CHECK(preset->alignment == 8);
    CHECK(preset->size == 88);
}

TEST_CASE("call: a string is no parameter or result call() passes", "[library][string-type][call]")
{
    std::string errors;
    auto program = fminus::Program::compile("int size(string s) { return string_size(s); }\n"
                                            "string name() { return \"name\"; }\n"
                                            "int named() { return size(name()); }\n"
                                            "int copy(char* out, int cap) { return string_copy(out, cap, name()); }\n",
                                            errors);
    INFO(errors);
    REQUIRE(program);
    CHECK(program->callable("size", errors) == nullptr);
    CHECK(errors.find("'size'") != std::string::npos);
    CHECK(errors.find("'string'") != std::string::npos);
    errors.clear();
    CHECK(program->callable("name", errors) == nullptr);
    CHECK(errors.find("'name' returns 'string'") != std::string::npos);
    errors.clear();
    // What wraps one is the host's to call, as any function of C types, a char* among them.
    auto* named = program->function<int()>("named", errors);
    REQUIRE(named != nullptr);
    CHECK(named() == 4);
    char out[8] = {};
    auto* copy = program->function<int(char*, int)>("copy", errors);
    REQUIRE(copy != nullptr);
    CHECK(copy(out, 8) == 4);
    CHECK(std::string(out) == "name");
}
