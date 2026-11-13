// tests/library/HandleTest.cpp: the handle types (http_response, json_doc, fft_plan, ...) as a host meets them (C++17,
// FMinus::FMinus alone): a module's handle field and array of handles as module() describes them, as a void* field is
// and an array of one is, with the handle's name for its type; and a function's handle parameter as callable()
// describes it and call() passes it, as a void*, which the host passes and a void* result it cannot.
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

TEST_CASE("a handle field is described as a void* field is, by its type's name", "[library][handles]")
{
    std::string errors;
    auto program = fminus::Program::compile("module Loader\n"
                                            "{\n"
                                            "    float level;\n"
                                            "    void* untyped;\n"
                                            "    fft_plan plan;\n"
                                            "    json_doc docs[4];\n"
                                            "    http_response pending[2][3];\n"
                                            "};\n",
                                            errors);
    INFO(errors);
    REQUIRE(program);
    const fminus::Program::Module* loader = program->module("Loader");
    REQUIRE(loader != nullptr);
    const auto* untyped = fieldNamed(*loader, "untyped");
    const auto* plan = fieldNamed(*loader, "plan");
    const auto* docs = fieldNamed(*loader, "docs");
    const auto* pending = fieldNamed(*loader, "pending");
    REQUIRE(untyped != nullptr);
    REQUIRE(plan != nullptr);
    REQUIRE(docs != nullptr);
    REQUIRE(pending != nullptr);
    CHECK(untyped->type == "void *");
    CHECK(plan->type == "fft_plan");
    CHECK(plan->offset == untyped->offset + sizeof(void*));
    CHECK(plan->size == untyped->size);
    CHECK(plan->element.empty());
    CHECK(plan->extents.empty());
    CHECK(docs->type == "json_doc[4]");
    CHECK(docs->element == "json_doc");
    CHECK(docs->extents == std::vector<std::size_t>{4});
    CHECK(docs->size == 4 * sizeof(void*));
    CHECK(pending->type == "http_response[2][3]");
    CHECK(pending->element == "http_response");
    CHECK(pending->extents == std::vector<std::size_t>{2, 3});
    CHECK(pending->offset == docs->offset + docs->size);
    CHECK(loader->size == pending->offset + 6 * sizeof(void*));
}

TEST_CASE("call: a handle parameter takes what a void* takes; a handle result is no value call() returns",
          "[library][handles][call]")
{
    std::string errors;
    auto program = fminus::Program::compile("int isNull(fft_plan p) { return p ? 0 : 1; }\n"
                                            "fft_plan make(int n) { return fft_create(n); }\n",
                                            errors);
    INFO(errors);
    REQUIRE(program);
    const fminus::Program::Callable* isNull = program->callable("isNull", errors);
    REQUIRE(isNull != nullptr);
    REQUIRE(isNull->parameters.size() == 1);
    CHECK(isNull->parameters[0].type == "fft_plan");
    CHECK(isNull->parameters[0].kind == fminus::Program::Value::Kind::Pointer);
    CHECK(isNull->parameters[0].element == fminus::Program::Value::Kind::Void);
    CHECK(isNull->parameters[0].module == nullptr);
    CHECK_FALSE(isNull->parameters[0].readOnly);

    float memory[4] = {};
    const fminus::Program::CallResult some = program->call(*isNull, {fminus::Program::Value::array(memory)});
    REQUIRE(some.status == fminus::Program::CallResult::Status::Ok);
    CHECK(some.value.number == 0.0);
    const fminus::Program::CallResult none =
        program->call(*isNull, {fminus::Program::Value::array(static_cast<float*>(nullptr))});
    REQUIRE(none.status == fminus::Program::CallResult::Status::Ok);
    CHECK(none.value.number == 1.0);

    CHECK(program->callable("make", errors) == nullptr);
    CHECK(errors.find("'make' returns 'fft_plan'") != std::string::npos);
}
