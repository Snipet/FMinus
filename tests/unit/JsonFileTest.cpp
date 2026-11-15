// tests/unit/JsonFileTest.cpp: a JSON literal as a file's bytes (runtime/Runtime.h, fminus_rt_file_write_json and
// fminus_rt_file_append_json), from C++, before any builtin calls them: the text the JSON writer makes of a
// description and its values, measured and then written once, as the whole file or, appended, with a newline after it
// (JSON Lines); a name with and without strings; the policy's size limit on the text (and on the newline appended),
// the quota of bytes written, a read-only folder and a null context; and a description the writer refuses, which is
// FILE_INVALID before the policy is asked. The descriptions are built as the compiler lays them out (Shapes.h).
//
// Everything happens in a directory each test makes under the build tree (beside bin/) and removes; nothing outside it
// is read, made, changed or removed. POSIX systems only, as FilesTest.cpp. In a build without file writing or the
// JSON writer nothing here is compiled.
#include "fminus/Config.h"

#if FMINUS_HAS_FILES_WRITE && FMINUS_HAS_JSON && !defined(_WIN32)
#include "Shapes.h"
#include "runtime/Files.h"
#include "runtime/Runtime.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <unistd.h>

namespace
{
    namespace fs = std::filesystem;
    using fminus::files::Context;
    using fminus::files::Policy;
    using fminus::test::shapes::Table;
    namespace kind = fminus::test::shapes;

    // A directory of the test's own beside bin/ (the build tree), removed when it goes (FilesTest.cpp's Scratch, under
    // json-file-test/).
    class Scratch
    {
    public:
        Scratch()
        {
            static std::atomic<int> made{0};
            fs::path tree;
#if defined(__linux__)
            std::error_code error;
            const fs::path self = fs::read_symlink("/proc/self/exe", error);
            if (!error)
                tree = self.parent_path().parent_path();
#endif
            if (tree.empty())
                tree = fs::current_path();
            path_ = tree / "json-file-test" / ("t" + std::to_string(::getpid()) + "-" + std::to_string(made++));
            fs::remove_all(path_);
            fs::create_directories(path_);
            path_ = fs::canonical(path_);
        }

        ~Scratch()
        {
            std::error_code ignored;
            fs::remove_all(path_, ignored);
            fs::remove(path_.parent_path(), ignored);
        }

        Scratch(const Scratch&) = delete;
        Scratch& operator=(const Scratch&) = delete;

        fs::path operator/(const std::string& name) const
        {
            return path_ / name;
        }

    private:
        fs::path path_;
    };

    std::string contents(const fs::path& file)
    {
        std::ifstream in(file, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }

    // A context over the scratch directory's "out" (written) and "kit" (read only), with `policy`'s limits.
    std::unique_ptr<Context> contextOver(const Scratch& scratch, Policy policy = {})
    {
        fs::create_directories(scratch / "out");
        fs::create_directories(scratch / "kit");
        policy.folders.push_back({"out", (scratch / "out").string(), true, {}});
        policy.folders.push_back({"kit", (scratch / "kit").string(), false, {}});
        std::string error;
        std::unique_ptr<Context> context = Context::create(policy, error);
        INFO(error);
        REQUIRE(context);
        return context;
    }

    // A day's summary, as a program's module is laid out: {"sensor": int, "mean": float, "peak": float}.
    struct Day
    {
        std::int32_t sensor;
        float mean;
        float peak;
    };

    // The literal {"days": <a span of Days>, "note": <string>}: its description and its values' slots.
    struct Literal
    {
        std::vector<std::int32_t> description;
        std::uint64_t slots[4] = {};

        Literal(const Day* days, std::int32_t count, std::string_view note)
        {
            Table t;
            t.literal(R"({"days":)");
            t.value(0, Table::span(8, sizeof(Day),
                                   t.module({{"sensor", offsetof(Day, sensor), Table::scalar(kind::Int)},
                                             {"mean", offsetof(Day, mean), Table::scalar(kind::Float)},
                                             {"peak", offsetof(Day, peak), Table::scalar(kind::Float)}})));
            t.literal(R"(,"note":)");
            t.value(2, Table::scalar(kind::String));
            t.literal("}");
            description = t.layout();
            std::memcpy(&slots[0], &days, sizeof days);
            std::memcpy(&slots[1], &count, sizeof count);
            const char* text = note.data();
            const auto length = static_cast<std::int32_t>(note.size());
            std::memcpy(&slots[2], &text, sizeof text);
            std::memcpy(&slots[3], &length, sizeof length);
        }
    };

    const Day kDays[2] = {{0, 0.5f, 1.25f}, {1, -0.125f, 2.0f}};
    constexpr std::string_view kText =
        R"({"days":[{"sensor":0,"mean":0.5,"peak":1.25},{"sensor":1,"mean":-0.125,"peak":2}],"note":"a \"day\""})";

    std::int32_t writeJson(void* context, std::string_view name, const Literal& literal, bool append,
                           const std::vector<std::string_view>& texts = {})
    {
        std::vector<const char*> addresses;
        std::vector<std::int32_t> lengths;
        for (const std::string_view t : texts)
        {
            addresses.push_back(t.data());
            lengths.push_back(static_cast<std::int32_t>(t.size()));
        }
        const auto length = static_cast<std::int32_t>(name.size());
        const auto count = static_cast<std::int32_t>(texts.size());
        return append ? fminus_rt_file_append_json(context, name.data(), length, nullptr, 0,
                                                   texts.empty() ? nullptr : addresses.data(),
                                                   texts.empty() ? nullptr : lengths.data(), count,
                                                   literal.description.data(), literal.slots)
                      : fminus_rt_file_write_json(context, name.data(), length, nullptr, 0,
                                                  texts.empty() ? nullptr : addresses.data(),
                                                  texts.empty() ? nullptr : lengths.data(), count,
                                                  literal.description.data(), literal.slots);
    }
} // namespace

TEST_CASE("a JSON literal written as a file is the writer's text, whole; appended, a line of it", "[runtime][files][json]")
{
    const Scratch scratch;
    const std::unique_ptr<Context> context = contextOver(scratch);
    const Literal literal(kDays, 2, "a \"day\"");
    CHECK(writeJson(context->handle(), "out/summary.json", literal, false) == FMINUS_FILE_OK);
    CHECK(contents(scratch / "out/summary.json") == kText);
    // Replaced whole by a shorter one: the count clamped to the span's capacity, and a count of 0.
    const Literal none(kDays, 0, "");
    CHECK(writeJson(context->handle(), "out/summary.json", none, false) == FMINUS_FILE_OK);
    CHECK(contents(scratch / "out/summary.json") == R"({"days":[],"note":""})");
    // Appended twice, by a name with a string: JSON Lines, each text and one newline.
    CHECK(writeJson(context->handle(), "out/%s.jsonl", literal, true, {"log"}) == FMINUS_FILE_OK);
    CHECK(writeJson(context->handle(), "out/%s.jsonl", none, true, {"log"}) == FMINUS_FILE_OK);
    CHECK(contents(scratch / "out/log.jsonl") == std::string(kText) + "\n" + R"({"days":[],"note":""})" + "\n");
    // A name with no %s and null texts is the name a plain write takes; a string the name's rules refuse is
    // FILE_INVALID, and nothing is written.
    CHECK(writeJson(context->handle(), "out/%s.json", literal, false, {".hidden"}) == FMINUS_FILE_INVALID);
    CHECK_FALSE(fs::exists(scratch / "out/.hidden.json"));
}

TEST_CASE("a JSON literal's text meets the policy's size limit and quota as bytes do, its newline among them",
          "[runtime][files][json]")
{
    const Scratch scratch;
    const Literal literal(kDays, 2, "a \"day\"");
    const auto size = static_cast<std::int32_t>(kText.size());
    {
        Policy policy;
        policy.maxFileBytes = size - 1;
        const std::unique_ptr<Context> context = contextOver(scratch, policy);
        CHECK(writeJson(context->handle(), "out/a.json", literal, false) == FMINUS_FILE_LIMIT);
        CHECK_FALSE(fs::exists(scratch / "out/a.json"));
    }
    {
        Policy policy;
        policy.maxFileBytes = size;   // the text fits; the text and its newline do not
        const std::unique_ptr<Context> context = contextOver(scratch, policy);
        CHECK(writeJson(context->handle(), "out/a.json", literal, false) == FMINUS_FILE_OK);
        CHECK(contents(scratch / "out/a.json") == kText);
        CHECK(writeJson(context->handle(), "out/a.jsonl", literal, true) == FMINUS_FILE_LIMIT);
        CHECK_FALSE(fs::exists(scratch / "out/a.jsonl"));
    }
    {
        Policy policy;
        policy.maxWrittenBytes = 2 * size + 1;   // a write and an append's text, and its newline: no more
        const std::unique_ptr<Context> context = contextOver(scratch, policy);
        CHECK(writeJson(context->handle(), "out/b.json", literal, false) == FMINUS_FILE_OK);
        CHECK(writeJson(context->handle(), "out/b.jsonl", literal, true) == FMINUS_FILE_OK);
        CHECK(writeJson(context->handle(), "out/c.json", literal, false) == FMINUS_FILE_LIMIT);
    }
}

TEST_CASE("a JSON literal to a file: the policy's refusals, a null context, and a description the writer refuses",
          "[runtime][files][json]")
{
    const Scratch scratch;
    Policy policy;
    policy.maxWrittenBytes = 93;   // the one literal below that is written, and no more
    const std::unique_ptr<Context> context = contextOver(scratch, policy);
    const Literal literal(kDays, 2, "x");
    CHECK(writeJson(context->handle(), "kit/a.json", literal, false) == FMINUS_FILE_REFUSED);   // read only
    CHECK(writeJson(context->handle(), "none/a.json", literal, true) == FMINUS_FILE_REFUSED);   // no such folder
    CHECK(writeJson(nullptr, "out/a.json", literal, false) == FMINUS_FILE_REFUSED);
    // Not well formed (a value of kind 99), or values with no slots: FILE_INVALID, before the policy is asked (a
    // folder it does not map is refused only after).
    Literal broken(kDays, 2, "x");
    broken.description = {3, 0, 1, 0, 99};
    CHECK(writeJson(context->handle(), "none/a.json", broken, false) == FMINUS_FILE_INVALID);
    CHECK(fminus_rt_file_append_json(context->handle(), "out/a.json", 10, nullptr, 0, nullptr, nullptr, 0,
                                     literal.description.data(), nullptr)
          == FMINUS_FILE_INVALID);
    CHECK_FALSE(fs::exists(scratch / "out/a.json"));
    // None of them counted against the quota: the literal's 93 bytes still fit it.
    REQUIRE(std::string_view(
                R"({"days":[{"sensor":0,"mean":0.5,"peak":1.25},{"sensor":1,"mean":-0.125,"peak":2}],"note":"x"})")
                .size()
            == 93);
    CHECK(writeJson(context->handle(), "out/a.json", literal, false) == FMINUS_FILE_OK);
}
#endif
