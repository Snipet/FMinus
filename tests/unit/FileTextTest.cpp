// tests/unit/FileTextTest.cpp: a file's name with strings in it (runtime/Runtime.h, fminus_rt_file_read_s, _list_s,
// _write_s, _append_s) and a listing's names (fminus_rt_file_name), from C++, before any builtin calls them: a %s joins
// the segment it is written in; the name filled in is checked at run time by the rules the compiler applies to a
// literal's segments, and one that breaks them, or a %s in the folder or the extension, is FILE_INVALID, never a trap;
// the policy's refusals stay FILE_REFUSED; a format with no %s is the entry point without strings; a listing's i-th
// name, the entry's own, read without allocating or a system call.
//
// Everything happens in a directory each test makes under the build tree (beside bin/) and removes; nothing outside it
// is read, made, changed or removed. POSIX systems only, as FilesTest.cpp; seccomp's part on Linux only.
#include "fminus/Config.h"

#if FMINUS_HAS_FILES && !defined(_WIN32)
#include "HeapCalls.h"
#include "StrictMode.h"
#include "runtime/Files.h"
#include "runtime/Runtime.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

namespace
{
    namespace fs = std::filesystem;
    using fminus::files::Context;
    using fminus::files::Policy;

    // A directory of the test's own beside bin/ (the build tree), removed when it goes (FilesTest.cpp's Scratch, under
    // files-text-test/).
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
            path_ = tree / "files-text-test" / ("t" + std::to_string(::getpid()) + "-" + std::to_string(made++));
            fs::remove_all(path_);
            for (int attempt = 0;; ++attempt)
            {
                std::error_code failed;
                fs::create_directories(path_, failed);
                if (!failed)
                    break;
                if (failed != std::errc::no_such_file_or_directory || attempt == 100)
                    fs::create_directories(path_);
            }
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

    void put(const fs::path& file, std::string_view bytes)
    {
        fs::create_directories(file.parent_path());
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }

    [[maybe_unused]] std::string contents(const fs::path& file)
    {
        std::ifstream in(file, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }

    bool buildTakes(std::initializer_list<std::string_view> types)
    {
        const std::string_view all = ";" FMINUS_FILES_TYPES ";";
        if (all == ";;")
            return true;
        for (const std::string_view type : types)
            if (all.find(";" + std::string(type) + ";") == std::string_view::npos)
                return false;
        return true;
    }

    // A context over the scratch directory's "kit" (read only) and "out" (written, in a build that writes).
    struct Fixture
    {
        Scratch scratch;
        std::unique_ptr<Context> context;

        Fixture()
        {
            put(scratch / "kit/pad1.f32", "pad1");
            put(scratch / "kit/warm pad.f32", "warm");
            put(scratch / "kit/sub/x.json", "{}");
            put(scratch / "kit/sub/y.json", "[]");
            put(scratch / "kit/caf\xc3\xa9.f32", "cafe");
            fs::create_directories(scratch / "out");
            Policy policy;
            policy.folders.push_back({"kit", (scratch / "kit").string(), false, {}});
            policy.folders.push_back({"out", (scratch / "out").string(), FMINUS_HAS_FILES_WRITE != 0, {}});
            std::string error;
            context = Context::create(policy, error);
            INFO(error);
            REQUIRE(context);
        }
    };

    struct Text
    {
        const char* bytes;
        std::int32_t length;
    };

    Text text(std::string_view t)
    {
        return {t.data(), static_cast<std::int32_t>(t.size())};
    }

    struct Arguments
    {
        std::vector<const char*> addresses;
        std::vector<std::int32_t> lengths;

        explicit Arguments(const std::vector<Text>& texts)
        {
            for (const Text& t : texts)
            {
                addresses.push_back(t.bytes);
                lengths.push_back(t.length);
            }
        }
        std::int32_t count() const
        {
            return static_cast<std::int32_t>(addresses.size());
        }
    };

    // A read of the format with these texts and ints: its status, and its bytes when it was read.
    std::int32_t readS(void* context, std::string_view format, const std::vector<Text>& texts,
                       const std::vector<std::int32_t>& ints = {}, std::string* bytes = nullptr)
    {
        const Arguments a(texts);
        void* file = fminus_rt_file_read_s(context, format.data(), static_cast<std::int32_t>(format.size()),
                                           ints.data(), static_cast<std::int32_t>(ints.size()), a.addresses.data(),
                                           a.lengths.data(), a.count());
        const std::int32_t status = fminus_rt_file_status(file);
        if (bytes != nullptr && status == FMINUS_FILE_OK)
            *bytes = std::string(static_cast<const char*>(fminus_rt_file_data(file)),
                                 static_cast<std::size_t>(fminus_rt_file_size(file)));
        fminus_rt_file_close(file);
        return status;
    }

    std::int32_t plainRead(void* context, std::string_view format, const std::vector<std::int32_t>& ints = {})
    {
        void* file = fminus_rt_file_read(context, format.data(), static_cast<std::int32_t>(format.size()), ints.data(),
                                         static_cast<std::int32_t>(ints.size()));
        const std::int32_t status = fminus_rt_file_status(file);
        fminus_rt_file_close(file);
        return status;
    }

    // A listing of the pattern with these texts: its status, and its names.
    std::int32_t listS(void* context, std::string_view pattern, const std::vector<Text>& texts,
                       std::vector<std::string>* names = nullptr)
    {
        const Arguments a(texts);
        void* list = fminus_rt_file_list_s(context, pattern.data(), static_cast<std::int32_t>(pattern.size()), nullptr,
                                           0, a.addresses.data(), a.lengths.data(), a.count());
        const std::int32_t status = fminus_rt_file_status(list);
        if (names != nullptr)
        {
            for (std::int32_t i = 0; i < fminus_rt_file_count(list); ++i)
            {
                std::int32_t length = -1;
                const char* name = fminus_rt_file_name(list, i, &length);
                names->emplace_back(name, static_cast<std::size_t>(length));
            }
        }
        fminus_rt_file_close(list);
        return status;
    }
} // namespace

TEST_CASE("a file name's %s joins the segment it is written in, and the name is then read as any is", "[runtime][files]")
{
    if (!buildTakes({"f32", "json"}))
        SKIP("this build leaves out f32 or json files");
    Fixture f;
    void* c = f.context->handle();
    std::string bytes;
    CHECK(readS(c, "kit/%s.f32", {text("pad1")}, {}, &bytes) == FMINUS_FILE_OK);
    CHECK(bytes == "pad1");
    CHECK(readS(c, "kit/%s.f32", {text("warm pad")}, {}, &bytes) == FMINUS_FILE_OK);
    CHECK(bytes == "warm");
    CHECK(readS(c, "kit/%s.f32", {text("caf\xc3\xa9")}, {}, &bytes) == FMINUS_FILE_OK);
    CHECK(bytes == "cafe");
    CHECK(readS(c, "kit/%s/%s.json", {text("sub"), text("y")}, {}, &bytes) == FMINUS_FILE_OK);
    CHECK(bytes == "[]");
    CHECK(readS(c, "kit/p%sd%d.f32", {text("a")}, {1}, &bytes) == FMINUS_FILE_OK);
    CHECK(bytes == "pad1");
    CHECK(readS(c, "kit/pad1%s.f32", {text("")}) == FMINUS_FILE_OK);    // an empty text, in a segment with more
    CHECK(readS(c, "kit/pad1%s.f32", {}) == FMINUS_FILE_OK);            // a %s past the last text: empty
    CHECK(readS(c, "kit/%s.f32", {text("pad2")}) == FMINUS_FILE_MISSING);
    CHECK(readS(c, "kit/%s/x.json", {text("none")}) == FMINUS_FILE_MISSING);
    CHECK(readS(c, "none/%s.f32", {text("pad1")}) == FMINUS_FILE_REFUSED);   // no such folder: the policy's
    CHECK(readS(nullptr, "kit/%s.f32", {text("pad1")}) == FMINUS_FILE_REFUSED);
}

TEST_CASE("a file name a string breaks is FILE_INVALID, and so is a %s in its folder or its extension",
          "[runtime][files]")
{
    if (!buildTakes({"f32", "json"}))
        SKIP("this build leaves out f32 or json files");
    Fixture f;
    void* c = f.context->handle();
    // What a string may not hold, or make its segment.
    for (const std::string_view bad :
         {std::string_view("a/b"), std::string_view("/"), std::string_view("*"), std::string_view("a\\b"),
          std::string_view("a:b"), std::string_view("a?"), std::string_view("a\"b"), std::string_view("a<b"),
          std::string_view("a|b"), std::string_view("\x01"), std::string_view("a\x7f"), std::string_view("\xc2\x85"),
          std::string_view("\xff"), std::string_view("\xc3"), std::string_view("\xed\xa0\x80"),
          std::string_view("a\0b", 3)})
    {
        INFO("'" << bad << "'");
        CHECK(readS(c, "kit/%s.f32", {text(bad)}) == FMINUS_FILE_INVALID);
        CHECK(readS(c, "kit/x%sy.f32", {text(bad)}) == FMINUS_FILE_INVALID);
    }
    CHECK(readS(c, "kit/%s/x.json", {text("")}) == FMINUS_FILE_INVALID);       // an empty segment
    CHECK(readS(c, "kit/%s.f32", {text("")}) == FMINUS_FILE_INVALID);          // ".f32": a leading "."
    CHECK(readS(c, "kit/%s.f32", {text(".hidden")}) == FMINUS_FILE_INVALID);
    CHECK(readS(c, "kit/%s/x.json", {text(".")}) == FMINUS_FILE_INVALID);
    CHECK(readS(c, "kit/%s/x.json", {text("..")}) == FMINUS_FILE_INVALID);
    CHECK(readS(c, "kit/%s/x.json", {text("sub.")}) == FMINUS_FILE_INVALID);   // a trailing "."
    CHECK(readS(c, "kit/%s/x.json", {text("sub ")}) == FMINUS_FILE_INVALID);   // a trailing space
    CHECK(readS(c, "kit/%s/x.json", {text(" sub")}) == FMINUS_FILE_INVALID);   // a leading one
    for (const std::string_view device : {"CON", "con", "Nul", "aux", "COM1", "lpt9", "com\xc2\xb9", "CONIN$"})
    {
        INFO(device);
        CHECK(readS(c, "kit/%s.f32", {text(device)}) == FMINUS_FILE_INVALID);
        CHECK(readS(c, "kit/%s/x.json", {text(device)}) == FMINUS_FILE_INVALID);
    }
    CHECK(readS(c, "kit/%s.f32", {text("COM10")}) == FMINUS_FILE_MISSING);    // no device
    // The lengths: 255 bytes a segment, 1024 a name.
    CHECK(readS(c, "kit/%s.f32", {text(std::string(251, 'a'))}) == FMINUS_FILE_MISSING);
    CHECK(readS(c, "kit/%s.f32", {text(std::string(252, 'a'))}) == FMINUS_FILE_INVALID);
    const std::string segment(250, 'a');
    CHECK(readS(c, "kit/%s/%s/%s/%s/x.json", {text(segment), text(segment), text(segment), text(segment)}) ==
          FMINUS_FILE_MISSING);   // 1014 bytes
    CHECK(readS(c, "kit/%s/%s/%s/%s/%s/x.json",
                {text(segment), text(segment), text(segment), text(segment), text(segment)}) == FMINUS_FILE_INVALID);
    CHECK(readS(c, "kit/%s.f32", {text(std::string(1 << 20, 'a'))}) == FMINUS_FILE_INVALID);
    // The folder and the extension are the literal's.
    CHECK(readS(c, "%s/pad1.f32", {text("kit")}) == FMINUS_FILE_INVALID);
    CHECK(readS(c, "k%s/pad1.f32", {text("it")}) == FMINUS_FILE_INVALID);
    CHECK(readS(c, "kit%s/pad1.f32", {text("")}) == FMINUS_FILE_INVALID);
    CHECK(readS(c, "kit/pad1.%s", {text("f32")}) == FMINUS_FILE_INVALID);
    CHECK(readS(c, "kit/pad1.f%s", {text("32")}) == FMINUS_FILE_INVALID);
    CHECK(readS(c, "kit/pad1%s", {text(".f32")}) == FMINUS_FILE_INVALID);
    CHECK(readS(c, "kit/pad%s.f32", {text("1.x")}) == FMINUS_FILE_MISSING);   // "pad1.x.f32": its type is f32
    CHECK(readS(c, "%s", {text("kit/pad1.f32")}) == FMINUS_FILE_INVALID);
    // An absolute name's root is the literal's too (and anywhere, a policy this one does not have).
    CHECK(readS(c, "%s/a/x.json", {text("")}) == FMINUS_FILE_INVALID);
    CHECK(readS(c, "/a/%s.json", {text("x")}) == FMINUS_FILE_REFUSED);
    // A text that is not one.
    CHECK(readS(c, "kit/%s.f32", {{"pad1", -1}}) == FMINUS_FILE_INVALID);
    CHECK(readS(c, "kit/%s.f32", {{nullptr, 4}}) == FMINUS_FILE_INVALID);
    CHECK(readS(c, "kit/pad1%s.f32", {{nullptr, 0}}) == FMINUS_FILE_OK);
}

TEST_CASE("a file name with no %s is the entry point's without strings", "[runtime][files]")
{
    if (!buildTakes({"f32", "json"}))
        SKIP("this build leaves out f32 or json files");
    Fixture f;
    void* c = f.context->handle();
    for (const std::string_view name : {"kit/pad%d.f32", "kit/pad1.f32", "kit/../pad1.f32", "kit/.pad%d.f32",
                                        "kit/pad.%d", "kit/CON.f32", "%d/pad1.f32", "kit/pad1", "kit/%%s.f32",
                                        "kit//pad1.f32", "none/pad1.f32", "kit/sub/%d.json"})
    {
        INFO(name);
        CHECK(readS(c, name, {text("unused")}, {1}) == plainRead(c, name, {1}));
    }
    CHECK(readS(c, "kit/pad%d.f32", {}, {1}) == FMINUS_FILE_OK);
    CHECK(plainRead(c, "kit/%s.f32") == FMINUS_FILE_MISSING);   // as text, a "%s"
}

TEST_CASE("a listing's pattern takes %s, and gives each file's own name", "[runtime][files]")
{
    if (!buildTakes({"f32", "json"}))
        SKIP("this build leaves out f32 or json files");
    Fixture f;
    void* c = f.context->handle();
    std::vector<std::string> names;
    CHECK(listS(c, "kit/%s/*.json", {text("sub")}, &names) == FMINUS_FILE_OK);
    CHECK(names == std::vector<std::string>{"x.json", "y.json"});
    names.clear();
    CHECK(listS(c, "kit/%s*.f32", {text("")}, &names) == FMINUS_FILE_OK);
    CHECK(names == std::vector<std::string>{"caf\xc3\xa9.f32", "pad1.f32", "warm pad.f32"});
    names.clear();
    CHECK(listS(c, "kit/%s*.f32", {text("pa")}, &names) == FMINUS_FILE_OK);
    CHECK(names == std::vector<std::string>{"pad1.f32"});
    CHECK(listS(c, "kit/%s.f32", {text("*")}) == FMINUS_FILE_INVALID);       // the "*" is the literal's
    CHECK(listS(c, "kit/%s/x.json", {text("sub")}) == FMINUS_FILE_INVALID);   // no "*": no pattern
    CHECK(listS(c, "kit/%s/*.json", {text("..")}) == FMINUS_FILE_INVALID);
    CHECK(listS(c, "%s/*.json", {text("kit")}) == FMINUS_FILE_INVALID);
    CHECK(listS(c, "kit/*.%s", {text("json")}) == FMINUS_FILE_INVALID);
    CHECK(listS(c, "kit/%s/*.json", {text("none")}) == FMINUS_FILE_MISSING);
    CHECK(listS(nullptr, "kit/%s/*.json", {text("sub")}) == FMINUS_FILE_REFUSED);

    // A name: null and 0 outside the listing, for a file, for null; `length` may be null.
    void* list = fminus_rt_file_list(c, "kit/sub/*.json", 14, nullptr, 0);
    REQUIRE(fminus_rt_file_count(list) == 2);
    std::int32_t length = -1;
    const char* first = fminus_rt_file_name(list, 0, &length);
    REQUIRE(first != nullptr);
    CHECK(std::string_view(first, static_cast<std::size_t>(length)) == "x.json");
    CHECK(first[length] == '\0');
    CHECK(fminus_rt_file_name(list, 1, nullptr) != nullptr);
    for (const std::int32_t index : {-1, 2, 1 << 30})
    {
        length = -1;
        CHECK(fminus_rt_file_name(list, index, &length) == nullptr);
        CHECK(length == 0);
    }
    void* file = fminus_rt_file_read(c, "kit/pad1.f32", 12, nullptr, 0);
    length = -1;
    CHECK(fminus_rt_file_name(file, 0, &length) == nullptr);
    CHECK(length == 0);
    CHECK(fminus_rt_file_name(nullptr, 0, &length) == nullptr);
    void* refused = fminus_rt_file_list(c, "none/*.json", 11, nullptr, 0);
    CHECK(fminus_rt_file_name(refused, 0, &length) == nullptr);
    fminus_rt_file_close(file);
    fminus_rt_file_close(list);
}

TEST_CASE("a listing's names are read without allocating or a system call", "[runtime][files]")
{
    if (!buildTakes({"f32", "json"}))
        SKIP("this build leaves out f32 or json files");
    Fixture f;
    void* list = fminus_rt_file_list(f.context->handle(), "kit/*.f32", 9, nullptr, 0);
    REQUIRE(fminus_rt_file_count(list) == 3);
    const auto readAll = [list]
    {
        std::int64_t sum = 0;
        for (std::int32_t i = -1; i <= fminus_rt_file_count(list); ++i)
        {
            std::int32_t length = 0;
            const char* name = fminus_rt_file_name(list, i, &length);
            sum += length + (name != nullptr ? name[0] : 1000);
        }
        return sum;
    };
    const std::int64_t expected = readAll();
    CHECK(expected == 9 + 'c' + 8 + 'p' + 12 + 'w' + 2000);
    if (fminus::test::HeapCalls::counted())
    {
        long calls = 0;
        bool same = true;
        {
            const fminus::test::HeapCalls counting;
            for (int i = 0; i < 100; ++i)
                same = same && readAll() == expected;
            calls = counting.count();
        }
        CHECK(same);
        CHECK(calls == 0);
    }
    else
        WARN("this build does not count the heap's calls (a sanitizer): allocation is not checked");
#if defined(__linux__)
    if (const fminus::test::Sealed<std::int64_t> sum = fminus::test::inStrictMode(readAll))
        CHECK(*sum == expected);
#endif
    fminus_rt_file_close(list);
}

#if FMINUS_HAS_FILES_WRITE
TEST_CASE("a file written or appended to by a name with %s, and the names refused", "[runtime][files]")
{
    if (!buildTakes({"f32", "json"}))
        SKIP("this build leaves out f32 or json files");
    Fixture f;
    void* c = f.context->handle();
    const auto writeS = [&](bool append, std::string_view format, const std::vector<Text>& texts,
                            std::string_view bytes, const std::vector<std::int32_t>& ints = {})
    {
        const Arguments a(texts);
        const auto length = static_cast<std::int32_t>(format.size());
        const auto n = static_cast<std::int32_t>(bytes.size());
        const auto count = static_cast<std::int32_t>(ints.size());
        return append ? fminus_rt_file_append_s(c, format.data(), length, ints.data(), count, a.addresses.data(),
                                                a.lengths.data(), a.count(), bytes.data(), n)
                      : fminus_rt_file_write_s(c, format.data(), length, ints.data(), count, a.addresses.data(),
                                               a.lengths.data(), a.count(), bytes.data(), n);
    };
    CHECK(writeS(false, "out/%s.json", {text("preset 1")}, "{\"a\":1}") == FMINUS_FILE_OK);
    CHECK(contents(f.scratch / "out/preset 1.json") == "{\"a\":1}");
    CHECK(writeS(true, "out/%s-%d.json", {text("log")}, "a", {2}) == FMINUS_FILE_OK);
    CHECK(writeS(true, "out/%s-%d.json", {text("log")}, "b", {2}) == FMINUS_FILE_OK);
    CHECK(contents(f.scratch / "out/log-2.json") == "ab");
    for (const bool append : {false, true})
    {
        INFO("append " << append);
        CHECK(writeS(append, "out/%s.json", {text("../x")}, "x") == FMINUS_FILE_INVALID);
        CHECK(writeS(append, "out/%s.json", {text(".x")}, "x") == FMINUS_FILE_INVALID);
        CHECK(writeS(append, "out/%s.json", {text("nul")}, "x") == FMINUS_FILE_INVALID);
        CHECK(writeS(append, "%s/x.json", {text("out")}, "x") == FMINUS_FILE_INVALID);
        CHECK(writeS(append, "out/x.%s", {text("json")}, "x") == FMINUS_FILE_INVALID);
        CHECK(writeS(append, "out/%s", {text("x.json")}, "x") == FMINUS_FILE_INVALID);
        CHECK(writeS(append, "out/%s.json", {{nullptr, 1}}, "x") == FMINUS_FILE_INVALID);
        CHECK(writeS(append, "kit/%s.json", {text("x")}, "x") == FMINUS_FILE_REFUSED);   // read only: the policy's
        CHECK(writeS(append, "out/%d.json", {}, "x", {7}) == FMINUS_FILE_OK);   // no %s: as ever
    }
    CHECK(contents(f.scratch / "out/7.json") == "xx");   // written, then appended to
    CHECK(!fs::exists(f.scratch / "x.json"));
    CHECK(fminus_rt_file_write_s(nullptr, "out/%s.json", 11, nullptr, 0, nullptr, nullptr, 0, "x", 1) ==
          FMINUS_FILE_REFUSED);
}
#endif
#endif
