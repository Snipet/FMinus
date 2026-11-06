// tests/library/FieldAttributeTest.cpp: a field's attributes as a host reads them (C++17, FMinus::FMinus alone):
// Module::Field::attributes and Parameter::attributes, in order, each with its name as written and its arguments,
// numbers (their values, and their spellings) and strings (their escapes resolved, UTF-8 kept), and none FMinus
// ignored with a warning; findAttribute, the last of a name; the same under the loop budget, bounds checks and #if,
// which leave a field's attributes alone.
#include "fminus/FMinus.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace
{
    using Program = fminus::Program;

    // A meter whose fields say how a host shows them: units, a label, a range, a display hint, a hidden table.
    constexpr const char* kMeter =
        "module Meter\n"
        "{\n"
        "    param float gain [[fminus::range(-24, 12)]] [[fminus::default(0)]] [[ui::unit(\"dB\")]];\n"
        "    float level [[ui::unit(\"dB\"), ui::label(\"Out\" \"put\")]] [[fminus::range(-60, 0)]];\n"
        "    float history [[ui::hidden]] [2][4];\n"
        "    float peak [[ui::display(\"meter\", 0x10, -1.5e1, 0x1p-3f)]]\n"
        "               [[ui::label(\"Peak\")]] [[ui::label(\"Hold\")]];\n"
        "    double time [[ui::unit(\"\\u00B5s\"), ui::format(\"%.1f\\t\\\"s\\\"\")]];\n"
        "    float plain;\n"
        "#ifdef DEBUG\n"
        "    float probe [[ui::label(\"/* debug */\")]];\n"
        "#endif\n"
        "    float tick(float x)\n"
        "    {\n"
        "        level = x * gain[0];\n"
        "        if (level > peak) peak = level;\n"
        "        history[1][3] = level;\n"
        "        return level;\n"
        "    }\n"
        "};\n";

    const Program::Module::Field& fieldNamed(const Program::Module& module, const std::string& name)
    {
        for (const Program::Module::Field& field : module.fields)
        {
            if (field.name == name)
                return field;
        }
        FAIL("no field " << name);
        return module.fields.front();
    }

    std::vector<std::string> namesOf(const std::vector<Program::Attribute>& attributes)
    {
        std::vector<std::string> names;
        for (const Program::Attribute& attribute : attributes)
            names.push_back(attribute.name);
        return names;
    }

    // Each check of the meter's attributes, for a program compiled with `options`.
    void checkMeter(const Program::Options& options, bool debug)
    {
        std::string errors;
        const std::unique_ptr<Program> program = Program::compile(kMeter, errors, options);
        REQUIRE(program);
        CHECK(errors.empty());
        const Program::Module* meter = program->module("Meter");
        REQUIRE(meter != nullptr);
        CHECK(meter->fields.size() == (debug ? 7u : 6u));

        // A field's, in order, two from one [[...]], with their arguments.
        const Program::Module::Field& level = fieldNamed(*meter, "level");
        CHECK(namesOf(level.attributes) == std::vector<std::string>{"ui::unit", "ui::label", "fminus::range"});
        REQUIRE(level.attributes[0].arguments.size() == 1);
        CHECK(level.attributes[0].arguments[0].isString);
        CHECK(level.attributes[0].arguments[0].text == "dB");
        CHECK(level.attributes[0].arguments[0].number == 0);
        CHECK(level.attributes[1].arguments[0].text == "Output");   // adjacent literals, one string
        REQUIRE(level.attributes[2].arguments.size() == 2);
        CHECK(!level.attributes[2].arguments[0].isString);
        CHECK(level.attributes[2].arguments[0].number == -60);
        CHECK(level.attributes[2].arguments[0].text == "-60");
        CHECK(level.attributes[2].arguments[1].number == 0);

        // A 2-D array's, without arguments.
        const Program::Module::Field& history = fieldNamed(*meter, "history");
        CHECK(history.type == "float[2][4]");
        CHECK(namesOf(history.attributes) == std::vector<std::string>{"ui::hidden"});
        CHECK(history.attributes[0].arguments.empty());

        // Numbers as C writes them, each with its value and its spelling; the last label is the one that counts.
        const Program::Module::Field& peak = fieldNamed(*meter, "peak");
        const Program::Attribute* display = Program::findAttribute(peak.attributes, "ui::display");
        REQUIRE(display != nullptr);
        REQUIRE(display->arguments.size() == 4);
        CHECK(display->arguments[0].text == "meter");
        CHECK(display->arguments[1].number == 16);
        CHECK(display->arguments[1].text == "0x10");
        CHECK(display->arguments[2].number == -15);
        CHECK(display->arguments[2].text == "-1.5e1");
        CHECK(display->arguments[3].number == 0.125);
        const Program::Attribute* label = Program::findAttribute(peak.attributes, "ui::label");
        REQUIRE(label != nullptr);
        CHECK(label->arguments[0].text == "Hold");
        CHECK(Program::findAttribute(peak.attributes, "ui::unit") == nullptr);

        // C's escapes resolved: a universal character name in UTF-8, a tab and quotes.
        const Program::Module::Field& time = fieldNamed(*meter, "time");
        CHECK(Program::findAttribute(time.attributes, "ui::unit")->arguments[0].text == "\xC2\xB5s");
        CHECK(Program::findAttribute(time.attributes, "ui::format")->arguments[0].text == "%.1f\t\"s\"");

        CHECK(fieldNamed(*meter, "plain").attributes.empty());

        // A parameter's are its field's, and its Parameter's, FMinus's own among them, which still give its range.
        const Program::Module::Field& gainField = fieldNamed(*meter, "gain");
        CHECK(gainField.type == "param float");
        CHECK(namesOf(gainField.attributes)
              == std::vector<std::string>{"fminus::range", "fminus::default", "ui::unit"});
        const Program::Parameter* gain = program->parameter("Meter::gain");
        REQUIRE(gain != nullptr);
        CHECK(namesOf(gain->attributes) == namesOf(gainField.attributes));
        CHECK(Program::findAttribute(gain->attributes, "ui::unit")->arguments[0].text == "dB");
        CHECK(gain->minimum == -24);
        CHECK(gain->maximum == 12);
        CHECK(gain->defaultValue == 0);

        if (debug)
            CHECK(Program::findAttribute(fieldNamed(*meter, "probe").attributes, "ui::label")->arguments[0].text
                  == "/* debug */");
    }
} // namespace

TEST_CASE("a host reads a field's attributes: names, numbers and strings, in order", "[library][fieldattr]")
{
    checkMeter({}, false);

    // A name without a namespace that C23 does not have, FMinus ignores with clang's warning, and the host never sees:
    // `range` here gives the parameter no range. C's standard attributes, and every namespace's, are passed on.
    std::string errors;
    const std::unique_ptr<Program> program = Program::compile(
        "module M { param float cutoff [[range(20, 20000)]] [[ui::unit(\"Hz\")]] [[deprecated]] [[ui::step(-0)]]; };",
        errors);
    REQUIRE(program);
    CHECK(errors.empty());
    CHECK(program->warnings()
          == "program.fm:1:33: warning: unknown attribute 'range' ignored [-Wunknown-attributes]\n"
             "program.fm:1:33: note: did you mean 'fminus::range'?\n");
    const Program::Parameter* cutoff = program->parameter("M::cutoff");
    REQUIRE(cutoff != nullptr);
    CHECK(namesOf(cutoff->attributes) == std::vector<std::string>{"ui::unit", "deprecated", "ui::step"});
    CHECK(namesOf(program->module("M")->fields.at(0).attributes) == namesOf(cutoff->attributes));
    CHECK(!cutoff->minimum);
    CHECK(!cutoff->maximum);
    // -0 is the integer 0, as C reads it, with no sign.
    CHECK(!std::signbit(Program::findAttribute(cutoff->attributes, "ui::step")->arguments.at(0).number));
}

TEST_CASE("the loop budget, bounds checks and #if leave a field's attributes as written", "[library][fieldattr]")
{
    Program::Options options;
    options.loopBudget = true;
    options.boundsCheck = true;
    options.defines = std::map<std::string, std::string>{{"DEBUG", "1"}};
    checkMeter(options, true);
}

TEST_CASE("a field FMinus refuses an attribute of is no program for a host", "[library][fieldattr]")
{
    std::string errors;
    CHECK(Program::compile("module M { float level [[fminus::default(1)]]; };", errors) == nullptr);
    CHECK(errors == "program.fm:1:26: error: 'fminus::default' attribute only applies to parameters: a field starts at "
                    "0, and a method such as init sets it\n");
}
