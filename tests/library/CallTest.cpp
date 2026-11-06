// tests/library/CallTest.cpp: calling a function by name, Program::callable and Program::call, as a host that learns a
// function's signature only as it runs uses them (C++17, FMinus::FMinus alone): each scalar type and a mix of them, as
// the function's own pointer computes them, bit for bit; arrays the host passes, written and read; a method on the
// host's instance; more arguments than the registers hold; the signature as values; each error as a value, never a
// crash; the loop budget and bounds checks stopping a call; calls from several threads at once; and the program's own
// code and IR as they were.
#include "fminus/FMinus.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <malloc.h>   // _aligned_malloc, _aligned_free
#endif

namespace
{
    using fminus::Program;
    using Value = Program::Value;
    using Kind = Program::Value::Kind;
    using Status = Program::CallResult::Status;

    // An instance of a module, as a host makes one: aligned, zeroed, the module's size (FMinus.h, Module).
    struct Instance
    {
        explicit Instance(const Program::Module& module)
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
            Program::setDefaults(memory, module);
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

    const char* const kSource = "float softclip(float x, float drive)\n"
                                "{\n"
                                "    const float y = x * drive;\n"
                                "    return y / (1.0f + fabsf(y));\n"
                                "}\n"
                                "int mix(int a, int b) { return a * 3 - b; }\n"
                                "bool positive(float x) { return x > 0.0f; }\n"
                                "double seconds(int samples, double rate) { return samples / rate; }\n"
                                "float pick(bool first, float a, float b) { return first ? a : b; }\n"
                                "void nothing(void) {}\n"
                                "void scale(float* restrict x, int n, float gain)\n"
                                "{\n"
                                "    for (int i = 0; i < n; ++i)\n"
                                "        x[i] *= gain;\n"
                                "}\n"
                                "double total(const double* x, int n)\n"
                                "{\n"
                                "    double sum = 0.0;\n"
                                "    for (int i = 0; i < n; ++i)\n"
                                "        sum += x[i];\n"
                                "    return sum;\n"
                                "}\n"
                                "void squares(int* out, int n)\n"
                                "{\n"
                                "    for (int i = 0; i < n; ++i)\n"
                                "        out[i] = i * i;\n"
                                "}\n"
                                "float second(const void* data, float x) { return x; }\n"
                                "float many(float a, int b, double c, bool d, float e, int f, double g, bool h,\n"
                                "           float i, int j, double k, bool l, float m, int n, double o, bool p,\n"
                                "           float q, int r, double s, bool t)\n"
                                "{\n"
                                "    return a + 2.0f * b + 3.0f * (float)c + (d ? 4.0f : 0.0f) + 5.0f * e\n"
                                "         + 6.0f * f + 7.0f * (float)g + (h ? 8.0f : 0.0f) + 9.0f * i + 10.0f * j\n"
                                "         + 11.0f * (float)k + (l ? 12.0f : 0.0f) + 13.0f * m + 14.0f * n\n"
                                "         + 15.0f * (float)o + (p ? 16.0f : 0.0f) + 17.0f * q + 18.0f * r\n"
                                "         + 19.0f * (float)s + (t ? 20.0f : 0.0f);\n"
                                "}\n"
                                "\n"
                                "module Osc\n"
                                "{\n"
                                "    param float frequency [[fminus::default(480)]];\n"
                                "    float phase;\n"
                                "    int ticks;\n"
                                "\n"
                                "    void reset(float to) { phase = to; ticks = 0; }\n"
                                "    float tick(float rate)\n"
                                "    {\n"
                                "        ticks += 1;\n"
                                "        phase = phase + frequency[0] / rate;\n"
                                "        while (phase >= 1.0f)\n"
                                "            phase = phase - 1.0f;\n"
                                "        return phase;\n"
                                "    }\n"
                                "};\n"
                                "\n"
                                "float advance(Osc* osc, float rate) { return osc->tick(rate); }\n"
                                "int ticksOf(const Osc* osc) { return osc->ticks; }\n";

    std::unique_ptr<Program> compiled(const char* source, const Program::Options& options = {})
    {
        std::string errors;
        auto program = Program::compile(source, errors, options);
        INFO(errors);
        REQUIRE(program != nullptr);
        return program;
    }

    std::uint32_t bitsOf(float value)
    {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof bits);
        return bits;
    }

    std::uint64_t bitsOf(double value)
    {
        std::uint64_t bits = 0;
        std::memcpy(&bits, &value, sizeof bits);
        return bits;
    }
} // namespace

TEST_CASE("call: each scalar type, and a mix, as the function's own pointer computes them", "[call]")
{
    auto program = compiled(kSource);
    std::string errors;

    auto* softclip = program->function<float(float, float)>("softclip", errors);
    REQUIRE(softclip != nullptr);
    for (const float x : {-3.0f, -0.1f, 0.0f, 0.37f, 2.5f, 1e-30f})
    {
        const Program::CallResult result = program->call("softclip", {Value::of(x), Value::of(4.0f)});
        REQUIRE(result.ok());
        CHECK(result.error.empty());
        CHECK(result.value.kind == Kind::Float);
        CHECK(bitsOf(static_cast<float>(result.value.number)) == bitsOf(softclip(x, 4.0f)));
    }

    const Program::CallResult mix = program->call("mix", {Value::of(7), Value::of(-2)});
    REQUIRE(mix.ok());
    CHECK(mix.value.kind == Kind::Int);
    CHECK(mix.value.number == 23.0);

    CHECK(program->call("positive", {Value::of(0.5f)}).value.number == 1.0);
    const Program::CallResult negative = program->call("positive", {Value::of(-0.5f)});
    CHECK(negative.value.kind == Kind::Bool);
    CHECK(negative.value.number == 0.0);

    auto* seconds = program->function<double(int, double)>("seconds", errors);
    const Program::CallResult clock = program->call("seconds", {Value::of(48001), Value::of(48000.0)});
    REQUIRE(clock.ok());
    CHECK(clock.value.kind == Kind::Double);
    CHECK(bitsOf(clock.value.number) == bitsOf(seconds(48001, 48000.0)));

    CHECK(program->call("pick", {Value::of(true), Value::of(1.5f), Value::of(2.5f)}).value.number == 1.5);
    CHECK(program->call("pick", {Value::of(false), Value::of(1.5f), Value::of(2.5f)}).value.number == 2.5);

    const Program::CallResult none = program->call("nothing", {});
    REQUIRE(none.ok());
    CHECK(none.value.kind == Kind::Void);
}

TEST_CASE("call: more arguments than the registers hold, every type among them", "[call]")
{
    auto program = compiled(kSource);
    std::string errors;
    using Many = float(float, int, double, bool, float, int, double, bool, float, int, double, bool, float, int, double,
                       bool, float, int, double, bool);
    auto* many = program->function<Many>("many", errors);
    REQUIRE(many != nullptr);
    const float expected = many(0.5f, 1, 0.25, true, 1.5f, -2, 0.75, false, 2.5f, 3, 1.25, true, 3.5f, -4, 1.75, false,
                                4.5f, 5, 2.25, true);
    const std::vector<Value> arguments{Value::of(0.5f), Value::of(1),     Value::of(0.25),  Value::of(true),
                                       Value::of(1.5f), Value::of(-2),    Value::of(0.75),  Value::of(false),
                                       Value::of(2.5f), Value::of(3),     Value::of(1.25),  Value::of(true),
                                       Value::of(3.5f), Value::of(-4),    Value::of(1.75),  Value::of(false),
                                       Value::of(4.5f), Value::of(5),     Value::of(2.25),  Value::of(true)};
    const Program::CallResult result = program->call("many", arguments);   // 20 slots: on the heap, past 16
    REQUIRE(result.ok());
    CHECK(bitsOf(static_cast<float>(result.value.number)) == bitsOf(expected));
}

TEST_CASE("call: arrays the host passes, written and read", "[call]")
{
    auto program = compiled(kSource);
    std::string errors;

    std::vector<float> samples{1.0f, -2.0f, 0.5f};
    const Program::CallResult scaled =
        program->call("scale", {Value::array(samples.data()), Value::of(3), Value::of(0.5f)});
    REQUIRE(scaled.ok());
    CHECK(scaled.value.kind == Kind::Void);
    CHECK(samples == std::vector<float>{0.5f, -1.0f, 0.25f});

    const std::vector<double> values{0.1, 0.2, 0.3};
    const Program::CallResult total = program->call("total", {Value::array(values.data()), Value::of(3)});
    REQUIRE(total.ok());
    CHECK(total.value.number == (0.1 + 0.2) + 0.3);
    std::vector<double> writable{1.0, 2.0};   // a pointer the function only reads through takes a writable one too
    CHECK(program->call("total", {Value::array(writable.data()), Value::of(2)}).value.number == 3.0);

    std::vector<int> squares(5, -1);
    REQUIRE(program->call("squares", {Value::array(squares.data()), Value::of(5)}).ok());
    CHECK(squares == std::vector<int>{0, 1, 4, 9, 16});

    // void* takes any pointer, a const one too, as its parameter is const; a module's instance as well.
    CHECK(program->call("second", {Value::array(values.data()), Value::of(2.0f)}).value.number == 2.0);
    Instance osc(*program->module("Osc"));
    CHECK(program->call("second", {Value::instance(osc.memory, *program->module("Osc")), Value::of(3.0f)}).ok());
    CHECK(program->call("second", {Value::array(static_cast<int*>(nullptr)), Value::of(4.0f)}).value.number == 4.0);

    // A const array where the function may write: C's "discards qualifiers", refused.
    const float* readOnly = samples.data();
    const Program::CallResult discards =
        program->call("scale", {Value::array(readOnly), Value::of(3), Value::of(2.0f)});
    CHECK(discards.status == Status::Mismatch);
    CHECK(discards.error == "argument 1 of 'scale' is 'const float *', not 'float *' (its parameter 'x')");
    CHECK(samples == std::vector<float>{0.5f, -1.0f, 0.25f});

    // Another element type.
    std::vector<int> integers(3);
    const Program::CallResult ints =
        program->call("scale", {Value::array(integers.data()), Value::of(3), Value::of(2.0f)});
    CHECK(ints.status == Status::Mismatch);
    CHECK(ints.error == "argument 1 of 'scale' is 'int *', not 'float *' (its parameter 'x')");

    // A pointer not aligned to its elements.
    alignas(8) unsigned char bytes[16] = {};
    float* odd = reinterpret_cast<float*>(bytes + 2);
    const Program::CallResult misaligned = program->call("scale", {Value::array(odd), Value::of(1), Value::of(2.0f)});
    CHECK(misaligned.status == Status::Mismatch);
    CHECK(misaligned.error
          == "argument 1 of 'scale' is not aligned to 4 bytes, as a 'float *' must be (its parameter 'x')");
}

TEST_CASE("call: a method runs on the host's instance, which keeps what it changes", "[call]")
{
    auto program = compiled(kSource);
    const Program::Module& module = *program->module("Osc");
    std::string errors;
    const Program::Callable* reset = program->callable("Osc::reset", errors);
    const Program::Callable* tick = program->callable("Osc::tick", errors);
    REQUIRE(reset != nullptr);
    REQUIRE(tick != nullptr);
    CHECK(errors.empty());

    Instance osc(module);   // frequency's default, 480
    const Value self = Value::instance(osc.memory, module);
    REQUIRE(program->call(*reset, {self, Value::of(0.5f)}).ok());
    const Program::CallResult first = program->call(*tick, {self, Value::of(960.0f)});
    REQUIRE(first.ok());
    CHECK(first.value.number == 0.0);   // 0.5 + 0.5 wraps
    CHECK(program->call(*tick, {self, Value::of(960.0f)}).value.number == 0.5);
    Program::setValue(osc.memory, *program->parameter("Osc::frequency"), 240.0);   // a knob the host sets
    CHECK(program->call(*tick, {self, Value::of(960.0f)}).value.number == 0.75);
    CHECK(program->call("ticksOf", {self}).value.number == 3.0);

    // Through a function that takes the module's pointer, and as its pointer of the method's own type computes.
    auto* direct = program->function<float(void*, float)>("Osc::tick", errors);
    Instance called(module);
    Instance pointed(module);
    const Value through = Value::instance(called.memory, module);
    for (int i = 0; i < 5; ++i)
        CHECK(program->call("advance", {through, Value::of(1000.0f)}).value.number
              == static_cast<double>(direct(pointed.memory, 1000.0f)));

    // A null instance: a method runs on one.
    const Program::CallResult null = program->call(*tick, {Value::instance(nullptr, module), Value::of(1.0f)});
    CHECK(null.status == Status::Mismatch);
    CHECK(null.error == "argument 1 of 'Osc::tick' is a null 'Osc *': a method runs on an instance (its parameter "
                        "'this')");

    // Another program's instance of the same layout is one; of another layout, or another module, it is not.
    auto same = compiled(kSource);
    Instance theirs(*same->module("Osc"));
    CHECK(program->call(*tick, {Value::instance(theirs.memory, *same->module("Osc")), Value::of(1.0f)}).ok());
    auto other = compiled("module Osc { float phase; float extra; float tick(float rate) { return phase; } };\n"
                          "module Lfo { float phase; };\n");
    Instance wider(*other->module("Osc"));
    const Program::CallResult laidOut =
        program->call(*tick, {Value::instance(wider.memory, *other->module("Osc")), Value::of(1.0f)});
    CHECK(laidOut.status == Status::Mismatch);
    CHECK(laidOut.error == "argument 1 of 'Osc::tick' is an instance of a module 'Osc' laid out differently from this "
                           "program's (another program's) (its parameter 'this')");
    Instance lfo(*other->module("Lfo"));
    const Program::CallResult lfoTick =
        program->call(*tick, {Value::instance(lfo.memory, *other->module("Lfo")), Value::of(1.0f)});
    CHECK(lfoTick.status == Status::Mismatch);
    CHECK(lfoTick.error == "argument 1 of 'Osc::tick' is 'Lfo *', not 'Osc *' (its parameter 'this')");
    CHECK(program->call(*tick, {Value::array(static_cast<float*>(osc.memory)), Value::of(1.0f)}).status
          == Status::Mismatch);
}

TEST_CASE("call: a Callable is the function's signature, as values", "[call]")
{
    auto program = compiled(kSource);
    std::string errors;

    const Program::Callable* seconds = program->callable("seconds", errors);
    REQUIRE(seconds != nullptr);
    CHECK(seconds->name == "seconds");
    CHECK(seconds->module == nullptr);
    CHECK(seconds->result == Kind::Double);
    CHECK(seconds->resultType == "double");
    REQUIRE(seconds->parameters.size() == 2);
    CHECK(seconds->parameters[0].name == "samples");
    CHECK(seconds->parameters[0].type == "int");
    CHECK(seconds->parameters[0].kind == Kind::Int);
    CHECK(seconds->parameters[1].kind == Kind::Double);
    CHECK(program->callable("seconds", errors) == seconds);   // the same each time

    const Program::Callable* total = program->callable("total", errors);
    REQUIRE(total != nullptr);
    CHECK(total->parameters[0].type == "const double *");
    CHECK(total->parameters[0].kind == Kind::Pointer);
    CHECK(total->parameters[0].element == Kind::Double);
    CHECK(total->parameters[0].readOnly);
    CHECK(total->parameters[0].module == nullptr);

    const Program::Callable* nothing = program->callable("nothing", errors);
    REQUIRE(nothing != nullptr);
    CHECK(nothing->result == Kind::Void);
    CHECK(nothing->resultType == "void");
    CHECK(nothing->parameters.empty());

    const Program::Callable* tick = program->callable("Osc::tick", errors);
    REQUIRE(tick != nullptr);
    CHECK(tick->module == program->module("Osc"));
    REQUIRE(tick->parameters.size() == 2);
    CHECK(tick->parameters[0].name == "this");
    CHECK(tick->parameters[0].type == "Osc *");
    CHECK(tick->parameters[0].kind == Kind::Pointer);
    CHECK(tick->parameters[0].element == Kind::Void);
    CHECK(tick->parameters[0].module == program->module("Osc"));
    CHECK_FALSE(tick->parameters[0].readOnly);

    const Program::Callable* ticks = program->callable("ticksOf", errors);
    REQUIRE(ticks != nullptr);
    CHECK(ticks->module == nullptr);
    CHECK(ticks->parameters[0].type == "const Osc *");
    CHECK(ticks->parameters[0].readOnly);

    const Program::Callable* second = program->callable("second", errors);
    REQUIRE(second != nullptr);
    CHECK(second->parameters[0].type == "const void *");
    CHECK(second->parameters[0].element == Kind::Void);
    CHECK(second->parameters[0].module == nullptr);
}

TEST_CASE("call: what goes wrong is a value, never a crash", "[call]")
{
    auto program = compiled("float softclip(float x, float drive) { return x * drive; }\n"
                            "float4 broadcast(float x) { return x; }\n"
                            "float first(float4 v) { return 1.0f; }\n"
                            "float* same(float* x) { return x; }\n"
                            "float lanes(float4* v) { return 1.0f; }\n"
                            "float nowhere(float x);\n"
                            "float atanf(float x);\n"
                            "float angle(float x) { return atanf(x); }\n");
    std::string errors;

    CHECK(program->callable("softclp", errors) == nullptr);
    CHECK(errors == "the program has no function 'softclp'\n");
    const Program::CallResult missing = program->call("softclp", {Value::of(1.0f), Value::of(2.0f)});
    CHECK(missing.status == Status::NoFunction);
    CHECK(missing.error == "the program has no function 'softclp'");
    CHECK(missing.value.kind == Kind::Void);
    CHECK(program->call("fabsf", {Value::of(1.0f)}).status == Status::NoFunction);   // a builtin is no function of its

    CHECK(program->callable("broadcast", errors) == nullptr);
    CHECK(errors == "'broadcast' returns 'float4'; call() returns nothing, or a bool, an int, a float or a double\n");
    const Program::CallResult vector = program->call("first", {Value::of(1.0f)});
    CHECK(vector.status == Status::Unsupported);
    CHECK(vector.error == "'first' takes a parameter of type 'float4'; call() passes bools, ints, floats, doubles and "
                          "pointers to ints, floats, doubles, modules and void only");
    CHECK(program->call("same", {Value::array(static_cast<float*>(nullptr))}).status == Status::Unsupported);
    CHECK(program->call("lanes", {Value::array(static_cast<float*>(nullptr))}).status == Status::Unsupported);

    // A prototype alone is no function of the program's: the C library's atanf, or one nothing defines.
    CHECK(program->callable("atanf", errors) == nullptr);
    CHECK(errors == "'atanf' is declared in the program but not defined there: call() calls the functions it "
                    "defines\n");
    CHECK(program->call("atanf", {Value::of(0.5f)}).status == Status::NoFunction);
    CHECK(program->call("angle", {Value::of(0.5f)}).value.number == static_cast<double>(std::atan(0.5f)));
    CHECK(program->call("nowhere", {Value::of(1.0f)}).status == Status::NoFunction);

    const Program::Callable* softclip = program->callable("softclip", errors);
    REQUIRE(softclip != nullptr);
    const Program::CallResult few = program->call(*softclip, {Value::of(1.0f)});
    CHECK(few.status == Status::Mismatch);
    CHECK(few.error == "'softclip' takes 2 arguments, not 1");
    CHECK(few.value.kind == Kind::Void);
    CHECK(program->call(*softclip, nullptr, 0).error == "'softclip' takes 2 arguments, not 0");
    CHECK(program->call("angle", {}).error == "'angle' takes 1 argument, not 0");
    const Program::CallResult type = program->call(*softclip, {Value::of(1.0f), Value::of(2)});
    CHECK(type.status == Status::Mismatch);
    CHECK(type.error == "argument 2 of 'softclip' is 'int', not 'float' (its parameter 'drive')");
    CHECK(program->call(*softclip, {Value::of(1.0), Value::of(2.0f)}).error
          == "argument 1 of 'softclip' is 'double', not 'float' (its parameter 'x')");
    CHECK(program->call(*softclip, {Value{}, Value::of(2.0f)}).error
          == "argument 1 of 'softclip' is 'void', not 'float' (its parameter 'x')");
    CHECK(program->call(*softclip, {Value::array(static_cast<float*>(nullptr)), Value::of(2.0f)}).error
          == "argument 1 of 'softclip' is 'float *', not 'float' (its parameter 'x')");

    // A Callable of another Program's.
    auto other = compiled("float softclip(float x, float drive) { return x; }\n");
    const Program::Callable* theirs = other->callable("softclip", errors);
    REQUIRE(theirs != nullptr);
    const Program::CallResult foreign = program->call(*theirs, {Value::of(1.0f), Value::of(2.0f)});
    CHECK(foreign.status == Status::Mismatch);
    CHECK(foreign.error == "'softclip' is not a Callable of this program's: callable() gives them");
    CHECK(other->call(*theirs, {Value::of(1.0f), Value::of(2.0f)}).value.number == 1.0);
}

TEST_CASE("call: Value::scalar converts a number as setValue converts a parameter's", "[call]")
{
    CHECK(Value::scalar(Kind::Float, 0.1).number == static_cast<double>(0.1f));
    CHECK(Value::scalar(Kind::Float, 0.1).kind == Kind::Float);
    CHECK(Value::scalar(Kind::Double, 0.1).number == 0.1);
    CHECK(Value::scalar(Kind::Int, 2.9).number == 2.0);
    CHECK(Value::scalar(Kind::Int, -2.9).number == -2.0);
    CHECK(Value::scalar(Kind::Int, 1e12).number == 2147483647.0);
    CHECK(Value::scalar(Kind::Int, -1e12).number == -2147483648.0);
    CHECK(Value::scalar(Kind::Int, std::nan("")).number == 0.0);
    CHECK(Value::scalar(Kind::Bool, 0.5).number == 1.0);
    CHECK(Value::scalar(Kind::Bool, 0.0).number == 0.0);
    CHECK(Value::scalar(Kind::Pointer, 1.0).kind == Kind::Void);
    CHECK(Value::scalar(Kind::Void, 1.0).kind == Kind::Void);

    auto program = compiled(kSource);
    std::string errors;
    const Program::Callable* mix = program->callable("mix", errors);
    REQUIRE(mix != nullptr);
    std::vector<Value> arguments;
    for (std::size_t i = 0; i < mix->parameters.size(); ++i)   // a host whose numbers are all doubles
        arguments.push_back(Value::scalar(mix->parameters[i].kind, i == 0 ? 4.7 : 1.0));
    CHECK(program->call(*mix, arguments).value.number == 11.0);
}

TEST_CASE("call: the loop budget and bounds checks stop a call, which says so", "[call]")
{
    const char* const source = "float spin(float x)\n"
                               "{\n"
                               "    while (x > 0.0f)\n"
                               "        x = x + 1.0f;\n"
                               "    return x;\n"
                               "}\n"
                               "double accumulate(int n)\n"
                               "{\n"
                               "    double sum = 0.0;\n"
                               "    for (int i = 0; i < n; ++i)\n"
                               "        sum += 0.5;\n"
                               "    return sum;\n"
                               "}\n"
                               "float tap(int i)\n"
                               "{\n"
                               "    const float taps[4] = {1, 2, 3, 4};\n"
                               "    return taps[i];\n"
                               "}\n"
                               "float grid(int r, int c)\n"
                               "{\n"
                               "    const float g[2][3] = {{1, 2, 3}, {4, 5, 6}};\n"
                               "    return g[r][c];\n"
                               "}\n";
    Program::Options options;
    options.loopBudget = true;
    options.boundsCheck = true;
    auto program = compiled(source, options);
    std::string errors;
    const Program::Callable* spin = program->callable("spin", errors);
    REQUIRE(spin != nullptr);

    program->setLoopBudget(1000);
    const Program::CallResult stopped = program->call(*spin, {Value::of(1.0f)});
    CHECK(stopped.status == Status::OutOfBudget);
    CHECK(stopped.error == "'spin' ran past its loop budget and returned early");
    CHECK(stopped.value.kind == Kind::Float);
    CHECK(stopped.value.number == 0.0);
    CHECK(program->loopBudgetExceeded());

    program->setLoopBudget(1000);
    const Program::CallResult done = program->call(*spin, {Value::of(-1.0f)});
    CHECK(done.ok());
    CHECK(done.value.number == -1.0);

    program->setLoopBudget(1000);   // a double in a budgeted loop
    CHECK(program->call("accumulate", {Value::of(100)}).value.number == 50.0);
    program->setLoopBudget(10);
    const Program::CallResult accumulated = program->call("accumulate", {Value::of(100000)});
    CHECK(accumulated.status == Status::OutOfBudget);
    CHECK(accumulated.value.kind == Kind::Double);
    CHECK(accumulated.value.number == 0.0);

    program->setLoopBudget(1000);
    CHECK(program->call("tap", {Value::of(3)}).value.number == 4.0);
    const Program::CallResult past = program->call("tap", {Value::of(4)});
    CHECK(past.status == Status::OutOfBounds);
    REQUIRE(program->boundsViolation());
    CHECK(past.error == "'tap' stopped: " + program->boundsViolation()->message());
    CHECK(past.error == "'tap' stopped: program.fm:17:16: index 4 out of bounds for 'taps' of type 'float[4]'");
    CHECK(past.value.number == 0.0);
    program->clearBoundsViolation();
    CHECK(program->call("tap", {Value::of(0)}).ok());

    // A 2-D array's index is checked in each dimension.
    CHECK(program->call("grid", {Value::of(1), Value::of(2)}).value.number == 6.0);
    const Program::CallResult row = program->call("grid", {Value::of(0), Value::of(3)});
    CHECK(row.status == Status::OutOfBounds);
    CHECK(row.error == "'grid' stopped: program.fm:22:16: index 3 out of bounds for 'g[r]' of type 'float[3]'");
    program->clearBoundsViolation();
}

TEST_CASE("call: from several threads at once, each on an instance of its own", "[call]")
{
    auto program = compiled(kSource);
    const Program::Module& module = *program->module("Osc");
    std::string errors;
    auto* direct = program->function<float(void*, float)>("Osc::tick", errors);
    REQUIRE(direct != nullptr);
    Instance reference(module);
    std::vector<float> expected;
    for (int i = 0; i < 1000; ++i)
        expected.push_back(direct(reference.memory, 44100.0f + static_cast<float>(i)));

    constexpr int kThreads = 4;
    std::vector<const Program::Callable*> found(kThreads, nullptr);
    std::vector<int> wrong(kThreads, 0);
    std::atomic<int> ready{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t)
        threads.emplace_back(
            [&, t]
            {
                ++ready;
                while (ready.load() < kThreads)
                    std::this_thread::yield();
                std::string mine;
                found[static_cast<std::size_t>(t)] = program->callable("Osc::tick", mine);   // compiled once
                if (found[static_cast<std::size_t>(t)] == nullptr)
                    return;
                Instance osc(module);
                const Value self = Value::instance(osc.memory, module);
                for (std::size_t i = 0; i < expected.size(); ++i)
                {
                    const Program::CallResult result = program->call(
                        *found[static_cast<std::size_t>(t)], {self, Value::of(44100.0f + static_cast<float>(i))});
                    if (!result.ok() || static_cast<float>(result.value.number) != expected[i])
                        ++wrong[static_cast<std::size_t>(t)];
                }
            });
    for (std::thread& thread : threads)
        thread.join();
    for (int t = 0; t < kThreads; ++t)
    {
        REQUIRE(found[static_cast<std::size_t>(t)] != nullptr);
        CHECK(found[static_cast<std::size_t>(t)] == found[0]);
        CHECK(wrong[static_cast<std::size_t>(t)] == 0);
    }
}

TEST_CASE("call: the program's own code and IR are as they were", "[call]")
{
    Program::Options options;
    options.keepIr = Program::KeepIr::Both;   // the JIT takes the module optimised already
    auto program = compiled(kSource, options);
    const std::string lowered = program->loweredIr();
    const std::string optimised = program->optimisedIr();
    std::string errors;
    auto* softclip = program->function<float(float, float)>("softclip", errors);
    const Program::CallResult result = program->call("softclip", {Value::of(0.75f), Value::of(3.0f)});
    REQUIRE(result.ok());
    CHECK(bitsOf(static_cast<float>(result.value.number)) == bitsOf(softclip(0.75f, 3.0f)));
    CHECK(program->loweredIr() == lowered);
    CHECK(program->optimisedIr() == optimised);
    CHECK(optimised.find("fminus.call") == std::string::npos);

    // A function an #ifdef keeps is one when the host defines the macro, and none when it does not.
    const char* const source = "#ifdef DEBUG\n"
                               "int level(void) { return 2; }\n"
                               "#endif\n"
                               "float gain(float x) { return x; }\n";
    Program::Options debug;
    debug.defines["DEBUG"] = "1";
    CHECK(compiled(source, debug)->call("level", {}).value.number == 2.0);
    CHECK(compiled(source)->call("level", {}).status == Status::NoFunction);
}
