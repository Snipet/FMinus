// tests/unit/CallThunkTest.cpp: the thunks that call a JIT'd function by its signature (jit/Call.h), which
// Program::call and fminus --run use: each type in its slot and back, a bool's byte read as C++ reads one, a result
// stored as its type in the slot's first bytes, arguments past the registers on the stack, and callType's map from
// FMinus's types.
#include "Compile.h"

#include "frontend/Ast.h"
#include "jit/Call.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

namespace
{
    using fminus::jit::CallSlot;
    using fminus::jit::CallType;

    fminus::jit::Thunk thunkOf(fminus::jit::Jit& jit, const char* name, CallType result,
                               const std::vector<CallType>& parameters)
    {
        REQUIRE(static_cast<bool>(jit.lookup(name)));
        auto thunk = fminus::jit::callThunk(jit, name, result, parameters);
        if (!thunk)
            FAIL(llvm::toString(thunk.takeError()));
        return *thunk;
    }
} // namespace

TEST_CASE("call thunks: each type in its slot, and the result in its own", "[jit][call]")
{
    auto jit = fminus::test::compile("float scaled(float x, int n, double d, bool b) { return b ? x * n : (float)d; }\n"
                                     "bool flip(bool b) { return !b; }\n"
                                     "int count(const float* x, int n)\n"
                                     "{\n"
                                     "    int k = 0;\n"
                                     "    for (int i = 0; i < n; ++i)\n"
                                     "        k += x[i] > 0.0f ? 1 : 0;\n"
                                     "    return k;\n"
                                     "}\n"
                                     "void fill(double* out, double v) { out[0] = v; out[1] = -v; }\n"
                                     "double sum12(double a, double b, double c, double d, double e, double f,\n"
                                     "             double g, double h, double i, int j, int k, double l)\n"
                                     "{\n"
                                     "    return a + 2.0 * b + 3.0 * c + 4.0 * d + 5.0 * e + 6.0 * f + 7.0 * g\n"
                                     "         + 8.0 * h + 9.0 * i + 10.0 * j + 11.0 * k + 12.0 * l;\n"
                                     "}\n");
    REQUIRE(jit != nullptr);

    const auto scaled = thunkOf(*jit, "scaled", CallType::Float,
                                {CallType::Float, CallType::Int, CallType::Double, CallType::Bool});
    CallSlot out;
    std::vector<CallSlot> args{CallSlot::of(1.5f), CallSlot::of(std::int32_t{-4}), CallSlot::of(0.25),
                               CallSlot::of(std::uint8_t{1})};
    scaled(args.data(), &out);
    CHECK(out.as<float>() == -6.0f);
    args[3] = CallSlot::of(std::uint8_t{0});
    scaled(args.data(), &out);
    CHECK(out.as<float>() == 0.25f);
    args[3] = CallSlot::of(std::uint64_t{0xFFFF'FFFF'FFFF'FF02});   // any byte but 0 is true
    scaled(args.data(), &out);
    CHECK(out.as<float>() == -6.0f);
    args[3] = CallSlot::of(std::uint64_t{0xFFFF'FFFF'FFFF'FF00});   // the bytes past the first are not read
    scaled(args.data(), &out);
    CHECK(out.as<float>() == 0.25f);

    const auto flip = thunkOf(*jit, "flip", CallType::Bool, {CallType::Bool});
    out = CallSlot::of(std::uint64_t{0xFFFF'FFFF'FFFF'FFFF});
    const CallSlot yes = CallSlot::of(std::uint8_t{1});
    flip(&yes, &out);
    CHECK(out.as<std::uint8_t>() == 0);   // a bool result is a byte, 0 or 1
    const CallSlot no = CallSlot::of(std::uint8_t{0});
    flip(&no, &out);
    CHECK(out.as<std::uint8_t>() == 1);

    const float samples[5] = {1.0f, -1.0f, 2.0f, 0.0f, 3.0f};
    const auto count = thunkOf(*jit, "count", CallType::Int, {CallType::Pointer, CallType::Int});
    const CallSlot countArgs[2] = {CallSlot::of(static_cast<const void*>(samples)), CallSlot::of(std::int32_t{5})};
    count(countArgs, &out);
    CHECK(out.as<std::int32_t>() == 3);

    double pair[2] = {0.0, 0.0};
    const auto fill = thunkOf(*jit, "fill", CallType::Void, {CallType::Pointer, CallType::Double});
    const CallSlot fillArgs[2] = {CallSlot::of(static_cast<void*>(pair)), CallSlot::of(0.5)};
    fill(fillArgs, &out);
    CHECK(pair[0] == 0.5);
    CHECK(pair[1] == -0.5);

    // Nine doubles fill arm64's and System V's eight vector registers and go on to the stack, among ints.
    std::vector<CallType> parameters(12, CallType::Double);
    parameters[9] = CallType::Int;
    parameters[10] = CallType::Int;
    const auto sum12 = thunkOf(*jit, "sum12", CallType::Double, parameters);
    std::vector<CallSlot> twelve;
    for (int i = 1; i <= 12; ++i)
        twelve.push_back(i == 10 || i == 11 ? CallSlot::of(std::int32_t{i}) : CallSlot::of(static_cast<double>(i)));
    sum12(twelve.data(), &out);
    CHECK(out.as<double>() == 650.0);   // the sum of i * i for i from 1 to 12

    // A second thunk of a function is LLVM's error, a duplicate definition, which the callers avoid.
    auto again = fminus::jit::callThunk(*jit, "flip", CallType::Bool, {CallType::Bool});
    CHECK_FALSE(static_cast<bool>(again));
    llvm::consumeError(again.takeError());
    CHECK(fminus::jit::thunkName("Osc::tick") == "fminus.call.Osc::tick");
}

TEST_CASE("call thunks: callType maps FMinus's types to what a thunk passes", "[jit][call]")
{
    using fminus::ast::Kind;
    using fminus::ast::Type;
    using fminus::jit::callType;
    CHECK(callType(Type(Kind::Void)) == CallType::Void);
    CHECK(callType(Type(Kind::Bool)) == CallType::Bool);
    CHECK(callType(Type(Kind::Int)) == CallType::Int);
    CHECK(callType(Type(Kind::Float)) == CallType::Float);
    CHECK(callType(Type(Kind::Double)) == CallType::Double);
    CHECK(callType(Type::pointerTo(Type(Kind::Float), true)) == CallType::Pointer);
    CHECK(callType(Type::pointerTo(Type(Kind::Float4), false)) == CallType::Pointer);
    CHECK_FALSE(callType(Type(Kind::Float4)).has_value());
    CHECK_FALSE(callType(Type(Kind::PolyFloat)).has_value());
    CHECK_FALSE(callType(Type(Kind::Bool8)).has_value());
    CHECK_FALSE(callType(Type::arrayOf(Type(Kind::Float), 4)).has_value());
}
