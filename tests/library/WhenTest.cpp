// tests/library/WhenTest.cpp: a module's `when` statements as a host meets them (C++17, FMinus::FMinus alone): each
// keeps a bool of the instance after the declared fields, which module() lists no field for, but which its size and
// alignment count, so an instance the host allocates by them holds it, and starts at 0 as the fields do.
#include "fminus/FMinus.h"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <memory>
#include <string>

TEST_CASE("a when's state is no field the host is told of, but the module's size counts it", "[library][when]")
{
    std::string errors;
    auto program = fminus::Program::compile("module Gate\n"
                                            "{\n"
                                            "    float level;\n"
                                            "    int count;\n"
                                            "    int step(bool on)\n"
                                            "    {\n"
                                            "        when (on) count++;\n"
                                            "        when (!on) level = 0;\n"
                                            "        return count;\n"
                                            "    }\n"
                                            "};\n",
                                            errors);
    INFO(errors);
    REQUIRE(program);
    const fminus::Program::Module* gate = program->module("Gate");
    REQUIRE(gate != nullptr);
    REQUIRE(gate->fields.size() == 2);
    CHECK(gate->fields[1].name == "count");
    CHECK(gate->fields[1].offset == 4);
    CHECK(gate->size == 12);   // two bools after the int, then padding to the float's alignment
    CHECK(gate->alignment == 4);

    auto* step = program->function<int(void*, bool)>("Gate::step", errors);
    REQUIRE(step != nullptr);
    const std::unique_ptr<std::byte[]> instance(new std::byte[gate->size]());   // zeroed, as FMinus starts one
    CHECK(step(instance.get(), true) == 1);
    CHECK(step(instance.get(), true) == 1);
    CHECK(step(instance.get(), false) == 1);
    CHECK(step(instance.get(), true) == 2);
}
