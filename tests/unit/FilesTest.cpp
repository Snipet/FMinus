// tests/unit/FilesTest.cpp: the file interface's runtime (runtime/Files.h, Runtime.h's fminus_rt_file_*), called from
// C++ as the builtins will call it: the policy and its check, the build's ceilings among its reasons; names, each the
// runtime refuses and each it does not; the folder, the walk and the deny-list, by identity; every trap built for real
// (links to outside, to inside, as a folder, as the file, mid-path; hard links; a FIFO; a directory where a file was
// named; a file that grows while it is read; one byte over the limit; a folder renamed; a denied directory reached by
// another route, made afterwards, renamed, swapped in and out while names are walked); a second name a file system
// answers to; closed handles; types; limits and quotas; listings; writes, failed at each step, and the temporary files
// of writes killed half way; the reads the audio thread may make, proved to allocate nothing and make no system call;
// an executable's environment. In a build with FMINUS_FILES_TYPES, a case whose files are of a type it leaves out is
// skipped (fixtures()).
//
// Everything happens in a directory each test makes under the build tree (beside bin/) and removes; nothing outside it
// is read, made, changed or removed, but for /dev/null and /proc, named only as what must be refused. POSIX systems
// only (links, FIFOs, hard links); seccomp's part and the swap races on Linux only.
#include "fminus/Config.h"

#if FMINUS_HAS_FILES && !defined(_WIN32)
#include "HeapCalls.h"
#include "runtime/Files.h"
#include "runtime/FilesSystem.h"
#include "runtime/Runtime.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__linux__)
#include <linux/seccomp.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#endif

namespace
{
    namespace fs = std::filesystem;
    using fminus::files::Context;
    using fminus::files::Policy;

    // A directory of the test's own beside bin/ (the build tree), removed when it goes, links and all (never followed).
    // Its parent, files-test/, is shared by the tests ctest runs at once, each a process: one removes it when it looks
    // empty, so another may find it gone between making it and making its own directory in it, and makes it again.
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
            path_ = tree / "files-test" / ("t" + std::to_string(::getpid()) + "-" + std::to_string(made++));
            fs::remove_all(path_);
            for (int attempt = 0;; ++attempt)
            {
                std::error_code failed;
                fs::create_directories(path_, failed);
                if (!failed)
                    break;
                if (failed != std::errc::no_such_file_or_directory || attempt == 100)
                    fs::create_directories(path_);   // throws, saying why
            }
            path_ = fs::canonical(path_);
        }
        ~Scratch()
        {
            std::error_code ignored;
            for (const fs::directory_entry& e : fs::recursive_directory_iterator(path_, ignored))
                if (!e.is_symlink())
                    ::chmod(e.path().c_str(), e.is_directory() ? 0755 : 0644);
            fs::remove_all(path_, ignored);
            fs::remove(path_.parent_path(), ignored);   // files-test/, when this was the last
        }
        Scratch(const Scratch&) = delete;
        Scratch& operator=(const Scratch&) = delete;

        const fs::path& path() const
        {
            return path_;
        }
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

    // Whether this build may touch files of each of `types` (FMINUS_FILES_TYPES; empty: of any). A case whose files
    // are of a type a build leaves out is skipped in it (fixtures()): FMINUS_FILES_TYPES's own refusals are the
    // policy's check's case here, scanFileName's in FileBuiltinsTest.cpp, and lit's files-types.fm.
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

    // Skips the case in a build whose FMINUS_FILES_TYPES leaves out one of the types its files have.
    void fixtures(std::initializer_list<std::string_view> types)
    {
        if (!buildTakes(types))
            SKIP("this build's FMINUS_FILES_TYPES (" FMINUS_FILES_TYPES ") leaves out a type the case's files have");
    }

    std::unique_ptr<Context> make(const Policy& policy)
    {
        std::string error;
        std::unique_ptr<Context> context = Context::create(policy, error);
        INFO(error);
        REQUIRE(context != nullptr);
        return context;
    }

    Policy::Folder folder(const std::string& name, const fs::path& directory, bool write = false,
                          std::vector<std::string> types = {})
    {
        Policy::Folder f;
        f.name = name;
        f.directory = directory.string();
        f.write = write;
        f.types = std::move(types);
        return f;
    }

    // The builtins as a program calls them: a name (a format) and its ints.
    void* read(void* context, std::string_view name, std::initializer_list<std::int32_t> ints = {})
    {
        return fminus_rt_file_read(context, name.data(), static_cast<std::int32_t>(name.size()), ints.begin(),
                                   static_cast<std::int32_t>(ints.size()));
    }

    void* list(void* context, std::string_view pattern, std::initializer_list<std::int32_t> ints = {})
    {
        return fminus_rt_file_list(context, pattern.data(), static_cast<std::int32_t>(pattern.size()), ints.begin(),
                                   static_cast<std::int32_t>(ints.size()));
    }

    std::int32_t status(const void* handle)
    {
        return fminus_rt_file_status(handle);
    }

    std::string bytesOf(const void* file)
    {
        const void* data = fminus_rt_file_data(file);
        return data == nullptr ? std::string() : std::string(static_cast<const char*>(data),
                                                             static_cast<std::size_t>(fminus_rt_file_size(file)));
    }

    // Reads `name` and closes it: its status, and its bytes in `out`.
    std::int32_t readStatus(void* context, std::string_view name, std::string* out = nullptr)
    {
        void* file = read(context, name);
        const std::int32_t s = status(file);
        if (out != nullptr)
            *out = bytesOf(file);
        fminus_rt_file_close(file);
        return s;
    }

#if FMINUS_HAS_FILES_WRITE
    std::int32_t write(void* context, std::string_view name, std::string_view bytes,
                       std::initializer_list<std::int32_t> ints = {})
    {
        return fminus_rt_file_write(context, name.data(), static_cast<std::int32_t>(name.size()), ints.begin(),
                                    static_cast<std::int32_t>(ints.size()), bytes.data(),
                                    static_cast<std::int32_t>(bytes.size()));
    }

    std::int32_t append(void* context, std::string_view name, std::string_view bytes)
    {
        return fminus_rt_file_append(context, name.data(), static_cast<std::int32_t>(name.size()), nullptr, 0,
                                     bytes.data(), static_cast<std::int32_t>(bytes.size()));
    }

    // Whether a directory holds a temporary file of a write's.
    bool temporaryLeft(const fs::path& directory)
    {
        for (const fs::directory_entry& e : fs::directory_iterator(directory))
            if (e.path().filename().string().starts_with(".fminus-"))
                return true;
        return false;
    }
#endif
} // namespace

TEST_CASE("the default policy grants nothing, and a null context has no files", "[runtime][files]")
{
    fixtures({"wav"});
    const Scratch scratch;
    put(scratch / "kit/a.wav", "RIFF");
    CHECK(fminus::files::checkPolicy(Policy{}).empty());
    const auto context = make(Policy{});
    CHECK(readStatus(context->handle(), "kit/a.wav") == FMINUS_FILE_REFUSED);
    CHECK(readStatus(nullptr, "kit/a.wav") == FMINUS_FILE_REFUSED);
    void* none = list(nullptr, "kit/*.wav");
    CHECK(status(none) == FMINUS_FILE_REFUSED);
    CHECK(fminus_rt_file_count(none) == 0);
    fminus_rt_file_close(none);
#if FMINUS_HAS_FILES_WRITE
    CHECK(write(nullptr, "kit/b.wav", "x") == FMINUS_FILE_REFUSED);
    CHECK(append(nullptr, "kit/b.wav", "x") == FMINUS_FILE_REFUSED);
#endif
    // A null handle, and one of the other kind, read as FILE_INVALID, 0 and null.
    CHECK(status(nullptr) == FMINUS_FILE_INVALID);
    CHECK(fminus_rt_file_size(nullptr) == 0);
    CHECK(fminus_rt_file_data(nullptr) == nullptr);
    CHECK(fminus_rt_file_count(nullptr) == 0);
    CHECK(status(fminus_rt_file_read_nth(nullptr, 0)) == FMINUS_FILE_INVALID);
    fminus_rt_file_close(nullptr);
    const std::uint32_t notAHandle[8] = {0x12345678, 0, 0, 0, 0, 0, 0, 0};
    CHECK(status(notAHandle) == FMINUS_FILE_INVALID);
    CHECK(context->held() == 0);
    CHECK(!fs::exists(scratch / "kit/b.wav"));
}

TEST_CASE("a file is read whole into memory, one 0 byte after it, and freed when closed", "[runtime][files]")
{
    fixtures({"f32"});
    const Scratch scratch;
    std::string bytes(100000, '\0');
    for (std::size_t i = 0; i < bytes.size(); ++i)
        bytes[i] = static_cast<char>(i * 7);
    put(scratch / "kit/tables/saw.f32", bytes);
    put(scratch / "kit/empty.f32", "");
    Policy policy;
    policy.folders = {folder("kit", scratch / "kit")};
    const auto context = make(policy);
    void* file = read(context->handle(), "kit/tables/saw.f32");
    REQUIRE(status(file) == FMINUS_FILE_OK);
    CHECK(fminus_rt_file_size(file) == 100000);
    CHECK(bytesOf(file) == bytes);
    CHECK(static_cast<const char*>(fminus_rt_file_data(file))[100000] == '\0');
    CHECK(fminus_rt_file_count(file) == 0);   // a file is no listing
    CHECK(context->held() == 1);
    CHECK(context->heldBytes() == 100000);
    void* empty = read(context->handle(), "kit/empty.f32");
    CHECK(status(empty) == FMINUS_FILE_OK);
    CHECK(fminus_rt_file_size(empty) == 0);
    CHECK(fminus_rt_file_data(empty) != nullptr);
    fminus_rt_file_close(file);
    fminus_rt_file_close(empty);
    CHECK(context->held() == 0);
    CHECK(context->heldBytes() == 0);
    // A name's %d places, filled in before it is checked.
    put(scratch / "kit/7.f32", "seven");
    put(scratch / "kit/bank-2/3.f32", "two three");
    std::string got;
    void* f = read(context->handle(), "kit/%d.f32", {7});
    CHECK(bytesOf(f) == "seven");
    fminus_rt_file_close(f);
    f = read(context->handle(), "kit/bank%d/%d.f32", {-2, 3});
    CHECK(bytesOf(f) == "two three");
    fminus_rt_file_close(f);
    CHECK(readStatus(context->handle(), "kit/%d.f32", nullptr) == FMINUS_FILE_REFUSED);   // no int: "kit/.f32"
    // Handles a program never closes go with the context.
    for (int i = 0; i < 5; ++i)
        read(context->handle(), "kit/7.f32");
    CHECK(context->held() == 5);
}

TEST_CASE("a closed handle reads as invalid until its place is taken, and closing it again does nothing",
          "[runtime][files]")
{
    // A closed handle's memory stays its context's, so a second close, or a read after the close, finds a closed
    // handle on any allocator (not memory a malloc reused, whose old kind word could still pass for a handle's).
    fixtures({"f32"});
    const Scratch scratch;
    put(scratch / "kit/a.f32", "abcd");
    put(scratch / "kit/b.f32", "efghij");
    Policy policy;
    policy.folders = {folder("kit", scratch / "kit")};
    const auto context = make(policy);
    void* c = context->handle();
    void* file = read(c, "kit/a.f32");
    void* listing = list(c, "kit/*.f32");
    REQUIRE(status(file) == FMINUS_FILE_OK);
    REQUIRE(fminus_rt_file_count(listing) == 2);
    CHECK(context->held() == 2);
    fminus_rt_file_close(file);
    fminus_rt_file_close(listing);
    CHECK(context->held() == 0);
    CHECK(context->heldBytes() == 0);
    CHECK(status(file) == FMINUS_FILE_INVALID);
    CHECK(fminus_rt_file_size(file) == 0);
    CHECK(fminus_rt_file_data(file) == nullptr);
    CHECK(status(listing) == FMINUS_FILE_INVALID);
    CHECK(fminus_rt_file_count(listing) == 0);
    CHECK(status(fminus_rt_file_read_nth(listing, 0)) == FMINUS_FILE_INVALID);
    fminus_rt_file_close(file);   // twice: nothing
    fminus_rt_file_close(listing);
    fminus_rt_file_close(file);
    CHECK(context->held() == 0);
    CHECK(context->heldBytes() == 0);
    // The next handles take the closed ones' places, and are whole handles of their own.
    void* again = read(c, "kit/b.f32");
    void* more = read(c, "kit/a.f32");
    CHECK(bytesOf(again) == "efghij");
    CHECK(bytesOf(more) == "abcd");
    CHECK(context->held() == 2);
    CHECK(context->heldBytes() == 10);
    fminus_rt_file_close(again);
    fminus_rt_file_close(more);
    CHECK(context->held() == 0);
    // Many reads and closes reuse a few places: held stays exact.
    for (int i = 0; i < 1000; ++i)
        fminus_rt_file_close(read(c, i % 2 == 0 ? "kit/a.f32" : "kit/b.f32"));
    CHECK(context->held() == 0);
    CHECK(context->heldBytes() == 0);
}

TEST_CASE("names the runtime refuses, and names it does not", "[runtime][files]")
{
    fixtures({"wav", "abcdefghijklmnop", "0"});
    const Scratch scratch;
    fs::create_directories(scratch / "kit/sub/deep");
    put(scratch / "kit/a.wav", "a");
    Policy policy;
    policy.folders = {folder("kit", scratch / "kit")};
    const auto context = make(policy);
    void* c = context->handle();
    const std::string longest(255 - 4, 'x');
    const std::vector<std::string> refused = {
        "", "kit", "kit/", "kit/a", "kit/a.", "kit//a.wav", "kit/a.wav/", "/kit/a.wav", "kit/../kit/a.wav",
        "kit/./a.wav", "kit/sub/../a.wav", "kit/..", "kit/.", "Kit/a.wav", "other/a.wav", "kit/.a.wav",
        "kit/.hidden/a.wav", "kit/a.wav.", "kit/a.wav ", "kit/ a.wav", "kit/sub /a.wav", "kit/sub./a.wav",
        "kit/a.w-v", "kit/a.w_v", "kit/a.abcdefghijklmnopq", "kit/a\\b.wav", "kit/a:b.wav", "kit/a.wav:x",
        "kit/a*b.wav", "kit/a?.wav", "kit/a\".wav", "kit/a<.wav", "kit/a>.wav", "kit/a|.wav", "kit/CON.wav",
        "kit/con.txt.wav", "kit/Nul.wav", "kit/PRN.wav", "kit/aux.wav", "kit/COM1.wav", "kit/com0.wav",
        "kit/lpt9.wav", "kit/COM\xc2\xb9.wav", "kit/LPT\xc2\xb2.wav", "kit/com\xc2\xb3.wav", "kit/CONIN$.wav",
        "kit/conout$.wav", "kit/NUL .wav", "kit/CON/a.wav", "kit/a\x01.wav", "kit/a\x1f.wav", "kit/a\x7f.wav",
        "kit/a\xc2\x80.wav", "kit/a\xc2\x9f.wav", "kit/a\xff.wav", "kit/a\xc0\xaf.wav", "kit/a\xe0\x80\xaf.wav",
        "kit/a\xed\xa0\x80.wav", "kit/a\xf4\x90\x80\x80.wav", "kit/a\xe2\x82.wav", "kit/a\xe2.wav",
        std::string("kit/a\0b.wav", 11), "kit/" + longest + "y.wav", "kit/a.wav\n", "kit/a.%d", "kit/a.w%d",
        "kit/%d", "kit/%d.wav"};
    for (const std::string& name : refused)
    {
        INFO("'" << name << "'");
        CHECK(readStatus(c, name) == FMINUS_FILE_REFUSED);
    }
    // More than 1024 bytes, or 16 segments, once filled in.
    std::string deep = "kit";
    for (int i = 0; i < 14; ++i)
        deep += "/d";
    CHECK(readStatus(c, deep + "/a.wav") == FMINUS_FILE_MISSING);           // 16 segments
    CHECK(readStatus(c, deep + "/d/a.wav") == FMINUS_FILE_REFUSED);         // 17
    std::string wide = "kit";
    while (wide.size() < 1024 - 6)
        wide += "/" + std::string(std::min<std::size_t>(200, 1024 - 6 - wide.size() - 1), 'w');
    REQUIRE(wide.size() == 1018);
    CHECK(readStatus(c, wide + "/a.wav") == FMINUS_FILE_MISSING);           // 1024 bytes
    CHECK(readStatus(c, wide + "/ab.wav") == FMINUS_FILE_REFUSED);          // 1025
    std::vector<std::int32_t> many(110, -2147483647 - 1);
    std::string format = "kit/";
    for (int i = 0; i < 110; ++i)
        format += "%d";
    format += ".wav";
    CHECK(status(fminus_rt_file_read(c, format.data(), static_cast<std::int32_t>(format.size()), many.data(), 110))
          == FMINUS_FILE_REFUSED);   // a short literal that its ints make longer than 1024 bytes
    const std::vector<std::string> allowed = {
        "kit/b.wav", "kit/a b.wav", "kit/a.b.wav", "kit/\xc3\xa9t\xc3\xa9.wav", "kit/COM10.wav", "kit/CONx.wav",
        "kit/com.wav", "kit/LPT.wav", "kit/xCON.wav", "kit/sub/deep/a.wav", "kit/a.WAV", "kit/x%%y.wav",
        "kit/a.abcdefghijklmnop", "kit/" + longest + ".wav", "kit/-1.wav", "kit/a..b.wav", "kit/a.b c.wav.wav",
        "kit/\xf0\x9f\x8e\xb9.wav", "kit/sub/a.0", "kit/%%d.wav", "kit/a%.wav", "kit/100%%.wav"};
    for (const std::string& name : allowed)
    {
        INFO("'" << name << "'");
        CHECK(readStatus(c, name) == FMINUS_FILE_MISSING);
    }
    CHECK(readStatus(c, "kit/a.wav") == FMINUS_FILE_OK);
}

TEST_CASE("types: the folder's list, else the policy's, in every letter case", "[runtime][files]")
{
    fixtures({"wav", "json", "f32", "exe"});
    const Scratch scratch;
    for (const char* name : {"a.wav", "b.WAV", "c.Wav", "d.json", "e.f32", "f.wav.exe", "g.exe.wav"})
        put(scratch / "kit" / name, "x");
    for (const char* name : {"a.json", "b.JSON", "c.wav"})
        put(scratch / "presets" / name, "{}");
    Policy policy;
    policy.folders = {folder("kit", scratch / "kit", false, {"WAV", "f32"}), folder("presets", scratch / "presets")};
    policy.types = {"Json"};
    const auto context = make(policy);
    void* c = context->handle();
    for (const char* name : {"kit/a.wav", "kit/b.WAV", "kit/c.Wav", "kit/e.f32", "kit/g.exe.wav", "presets/a.json",
                             "presets/b.JSON"})
    {
        INFO(name);
        CHECK(readStatus(c, name) == FMINUS_FILE_OK);
    }
    for (const char* name : {"kit/d.json", "kit/f.wav.exe", "presets/c.wav", "presets/missing.wav"})
    {
        INFO(name);
        CHECK(readStatus(c, name) == FMINUS_FILE_REFUSED);   // refused by its type before anything is opened
    }
    CHECK(readStatus(c, "kit/missing.wav") == FMINUS_FILE_MISSING);
}

TEST_CASE("symbolic links are refused wherever they are met", "[runtime][files]")
{
    fixtures({"wav"});
    const Scratch scratch;
    put(scratch / "outside/secret.wav", "SECRET");
    put(scratch / "outside/sub/a.wav", "SECRET");
    put(scratch / "kit/real.wav", "inside");
    put(scratch / "kit/sub/a.wav", "inside");
    fs::create_symlink(scratch / "outside/secret.wav", scratch / "kit/out.wav");     // to outside
    fs::create_symlink("real.wav", scratch / "kit/in.wav");                          // to inside, relative
    fs::create_symlink(scratch / "kit/real.wav", scratch / "kit/in2.wav");           // to inside, absolute
    fs::create_directory_symlink(scratch / "outside", scratch / "kit/door");         // as a folder segment
    fs::create_directory_symlink("sub", scratch / "kit/alias");                      // a directory inside
    fs::create_directory_symlink("../outside/sub", scratch / "kit/sub/up");          // mid-path
    fs::create_symlink("nowhere.wav", scratch / "kit/dangling.wav");
    fs::create_symlink("loop.wav", scratch / "kit/loop.wav");
    Policy policy;
    policy.folders = {folder("kit", scratch / "kit")};
    const auto context = make(policy);
    void* c = context->handle();
    for (const char* name : {"kit/out.wav", "kit/in.wav", "kit/in2.wav", "kit/door/secret.wav", "kit/door/sub/a.wav",
                             "kit/alias/a.wav", "kit/sub/up/a.wav", "kit/dangling.wav", "kit/loop.wav"})
    {
        INFO(name);
        std::string got;
        CHECK(readStatus(c, name, &got) == FMINUS_FILE_REFUSED);
        CHECK(got.empty());
    }
    CHECK(readStatus(c, "kit/real.wav") == FMINUS_FILE_OK);
    // The folder's own directory is the host's text: a link there is followed, once, when the context is made.
    fs::create_directory_symlink(scratch / "kit", scratch / "kitlink");
    Policy linked;
    linked.folders = {folder("kit", scratch / "kitlink")};
    const auto viaLink = make(linked);
    std::string got;
    CHECK(readStatus(viaLink->handle(), "kit/real.wav", &got) == FMINUS_FILE_OK);
    CHECK(got == "inside");
}

TEST_CASE("a segment is the entry's own name, not a second name a file system answers to", "[runtime][files]")
{
    // FAT and NTFS also find an entry by its 8.3 short name ("SSH~1" for ".ssh", "NOTES~1.WAV" for "notes.wavx"), and
    // a Windows share, WSL's drives or a FUSE server may answer to anything: a name the rules pass could then reach a
    // hidden file, or one of a type not allowed. On every file system but the native ones of Linux and macOS, each
    // segment is checked against the directory's listing (nameListed). None of those can be mounted here, so the check
    // is shown on its own, and then made of every name on this file system (checkEveryName), where it must change
    // nothing.
    fixtures({"wav"});
    const Scratch scratch;
    put(scratch / "kit/.ssh/key.wav", "SECRET");
    put(scratch / "kit/Notes.wav", "notes");
    put(scratch / "kit/notes.wavx", "NOT A WAV");
    put(scratch / "kit/sub/a.wav", "a");
    using fminus::files::os::nameListed;
    fminus::files::os::Dir kit;
    std::string why;
    REQUIRE(fminus::files::os::openFolder((scratch / "kit").string(), kit, why) == FMINUS_FILE_OK);
    CHECK(nameListed(kit.get(), "Notes.wav") == FMINUS_FILE_OK);
    CHECK(nameListed(kit.get(), "NOTES.WAV") == FMINUS_FILE_OK);   // letter case aside
    CHECK(nameListed(kit.get(), "sub") == FMINUS_FILE_OK);
    CHECK(nameListed(kit.get(), ".ssh") == FMINUS_FILE_OK);
    CHECK(nameListed(kit.get(), "SSH~1") == FMINUS_FILE_REFUSED);
    CHECK(nameListed(kit.get(), "NOTES~1.WAV") == FMINUS_FILE_REFUSED);
    CHECK(nameListed(kit.get(), "notes.wav") == FMINUS_FILE_OK);
    CHECK(nameListed(kit.get(), "notes.wa") == FMINUS_FILE_REFUSED);
    CHECK(nameListed(kit.get(), "Notes.wav ") == FMINUS_FILE_REFUSED);

    struct Every
    {
        Every()
        {
            fminus::files::os::checkEveryName(true);
        }
        ~Every()
        {
            fminus::files::os::checkEveryName(false);
        }
    } every;
    Policy policy;
    policy.folders = {folder("kit", scratch / "kit", FMINUS_HAS_FILES_WRITE != 0)};
    const auto context = make(policy);
    void* c = context->handle();
    std::string got;
    CHECK(readStatus(c, "kit/Notes.wav", &got) == FMINUS_FILE_OK);
    CHECK(got == "notes");
    CHECK(readStatus(c, "kit/sub/a.wav", &got) == FMINUS_FILE_OK);
    CHECK(got == "a");
    CHECK(readStatus(c, "kit/sub/none.wav") == FMINUS_FILE_MISSING);
    CHECK(readStatus(c, "kit/none/a.wav") == FMINUS_FILE_MISSING);
    void* all = list(c, "kit/sub/*.wav");
    REQUIRE(fminus_rt_file_count(all) == 1);
    void* nth = fminus_rt_file_read_nth(all, 0);
    CHECK(bytesOf(nth) == "a");
    fminus_rt_file_close(nth);
    fminus_rt_file_close(all);
#if FMINUS_HAS_FILES_WRITE
    CHECK(write(c, "kit/sub/b.wav", "b") == FMINUS_FILE_OK);
    CHECK(write(c, "kit/sub/b.wav", "bb") == FMINUS_FILE_OK);
    CHECK(append(c, "kit/sub/b.wav", "c") == FMINUS_FILE_OK);
    CHECK(append(c, "kit/sub/new.wav", "n") == FMINUS_FILE_OK);
    CHECK(contents(scratch / "kit/sub/b.wav") == "bbc");
    CHECK(contents(scratch / "kit/sub/new.wav") == "n");
#endif
}

TEST_CASE("a hard link is read unless the policy asks for one link only", "[runtime][files]")
{
    fixtures({"wav"});
    const Scratch scratch;
    put(scratch / "outside/secret.wav", "SECRET");
    put(scratch / "kit/plain.wav", "plain");
    fs::create_hard_link(scratch / "outside/secret.wav", scratch / "kit/hard.wav");
    Policy policy;
    policy.folders = {folder("kit", scratch / "kit")};
    std::string got;
    CHECK(readStatus(make(policy)->handle(), "kit/hard.wav", &got) == FMINUS_FILE_OK);
    CHECK(got == "SECRET");   // what a hard link is: the same file, which singleLink is for
    policy.singleLink = true;
    const auto single = make(policy);
    CHECK(readStatus(single->handle(), "kit/hard.wav", &got) == FMINUS_FILE_REFUSED);
    CHECK(readStatus(single->handle(), "kit/plain.wav") == FMINUS_FILE_OK);
}

TEST_CASE("a FIFO, a directory and a file where a directory is walked are refused, at once", "[runtime][files]")
{
    fixtures({"wav"});
    const Scratch scratch;
    fs::create_directories(scratch / "kit/dir.wav");
    REQUIRE(::mkfifo((scratch / "kit/pipe.wav").c_str(), 0644) == 0);
    REQUIRE(::mkfifo((scratch / "kit/pipedir").c_str(), 0644) == 0);
    put(scratch / "kit/file.wav", "x");
    Policy policy;
    policy.folders = {folder("kit", scratch / "kit")};
    const auto context = make(policy);
    const auto start = std::chrono::steady_clock::now();
    CHECK(readStatus(context->handle(), "kit/pipe.wav") == FMINUS_FILE_REFUSED);       // never waits for a writer
    CHECK(readStatus(context->handle(), "kit/pipedir/a.wav") == FMINUS_FILE_REFUSED);
    CHECK(readStatus(context->handle(), "kit/dir.wav") == FMINUS_FILE_REFUSED);
    CHECK(readStatus(context->handle(), "kit/file.wav/a.wav") == FMINUS_FILE_REFUSED);
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(5));
    // A writer waiting on the FIFO would block here forever if the runtime had opened it for reading.
    const int fd = ::open((scratch / "kit/pipe.wav").c_str(), O_WRONLY | O_NONBLOCK);
    CHECK(fd < 0);   // ENXIO: no reader has it open
    if (fd >= 0)
        ::close(fd);
    // A folder that is no directory is the host's mistake, said when the context is made.
    Policy wrong;
    wrong.folders = {folder("kit", scratch / "kit/file.wav")};
    std::string error;
    CHECK(Context::create(wrong, error) == nullptr);
    CHECK(error.starts_with("folders: 'kit': '" + (scratch / "kit/file.wav").string() + "' cannot be opened: "));
    wrong.folders = {folder("kit", scratch / "nothing")};
    CHECK(Context::create(wrong, error) == nullptr);
    CHECK(error.find("cannot be opened") != std::string::npos);
}

TEST_CASE("limits: the file's size, the files held, the bytes held", "[runtime][files]")
{
    fixtures({"f32"});
    const Scratch scratch;
    put(scratch / "kit/full.f32", std::string(1000, 'f'));
    put(scratch / "kit/over.f32", std::string(1001, 'o'));
    put(scratch / "kit/half.f32", std::string(500, 'h'));
    Policy policy;
    policy.folders = {folder("kit", scratch / "kit")};
    policy.maxFileBytes = 1000;
    policy.maxOpen = 2;
    policy.maxHeldBytes = 1500;
    const auto context = make(policy);
    void* c = context->handle();
    CHECK(readStatus(c, "kit/over.f32") == FMINUS_FILE_LIMIT);   // one byte over
    void* a = read(c, "kit/full.f32");
    CHECK(status(a) == FMINUS_FILE_OK);
    void* b = read(c, "kit/full.f32");
    CHECK(status(b) == FMINUS_FILE_LIMIT);   // 2000 bytes held would be over 1500
    void* d = read(c, "kit/half.f32");
    CHECK(status(d) == FMINUS_FILE_OK);   // 1500 exactly
    void* e = read(c, "kit/half.f32");
    CHECK(status(e) == FMINUS_FILE_LIMIT);   // a third handle (and bytes besides)
    void* l = list(c, "kit/*.f32");
    CHECK(status(l) == FMINUS_FILE_LIMIT);   // a listing takes a place too
    CHECK(context->held() == 2);             // what failed holds nothing
    fminus_rt_file_close(a);
    fminus_rt_file_close(b);   // a shared failure: closing it does nothing
    fminus_rt_file_close(d);
    fminus_rt_file_close(e);
    CHECK(context->held() == 0);
    CHECK(context->heldBytes() == 0);
    l = list(c, "kit/*.f32");
    CHECK(status(l) == FMINUS_FILE_OK);
    fminus_rt_file_close(l);
}

TEST_CASE("a file that grows while it is read gives what was there, within the limit, or the limit", "[runtime][files]")
{
    fixtures({"f32"});
    const Scratch scratch;
    put(scratch / "kit/grow.f32", std::string(4096, 'g'));
    Policy policy;
    policy.folders = {folder("kit", scratch / "kit")};
    policy.maxFileBytes = 4 << 20;
    const auto context = make(policy);
    std::thread writer([&] {
        const int fd = ::open((scratch / "kit/grow.f32").c_str(), O_WRONLY | O_APPEND | O_CLOEXEC);
        const std::string block(65536, 'g');
        while (fd >= 0)
        {
            if (::write(fd, block.data(), block.size()) < 0)
                break;
            struct stat st;
            if (::fstat(fd, &st) == 0 && st.st_size > (8 << 20))
                break;
        }
        if (fd >= 0)
            ::close(fd);
    });
    int ok = 0;
    int limit = 0;
    for (int i = 0; i < 400; ++i)
    {
        void* file = read(context->handle(), "kit/grow.f32");
        const std::int32_t s = status(file);
        if (s == FMINUS_FILE_OK)
        {
            ++ok;
            CHECK(fminus_rt_file_size(file) <= (4 << 20));
            CHECK(fminus_rt_file_size(file) >= 4096);
        }
        else
        {
            CHECK(s == FMINUS_FILE_LIMIT);
            ++limit;
        }
        fminus_rt_file_close(file);
    }
    writer.join();   // it stops once the file is past 8 MiB
    CHECK(ok + limit == 400);
    CHECK(readStatus(context->handle(), "kit/grow.f32") == FMINUS_FILE_LIMIT);   // over 4 MiB now
}

TEST_CASE("a folder renamed after the policy was made is still the folder", "[runtime][files]")
{
    fixtures({"wav"});
    const Scratch scratch;
    put(scratch / "kit/a.wav", "original");
    Policy policy;
    policy.folders = {folder("kit", scratch / "kit")};
    const auto context = make(policy);
    fs::rename(scratch / "kit", scratch / "moved");
    put(scratch / "kit/a.wav", "impostor");   // a new directory where the folder was
    std::string got;
    CHECK(readStatus(context->handle(), "kit/a.wav", &got) == FMINUS_FILE_OK);
    CHECK(got == "original");
    fs::remove_all(scratch / "moved");
    CHECK(readStatus(context->handle(), "kit/a.wav") != FMINUS_FILE_OK);   // gone: never the impostor
}

TEST_CASE("the deny-list wins by identity, however a name reaches the directory", "[runtime][files]")
{
    fixtures({"wav"});
    const Scratch scratch;
    put(scratch / "kit/open.wav", "open");
    put(scratch / "kit/private/a.wav", "PRIVATE");
    put(scratch / "kit/private/deep/b.wav", "PRIVATE");
    put(scratch / "kit/key.wav", "KEY");
    put(scratch / "kit/sub/c.wav", "fine");
    fs::create_directory_symlink(scratch / "kit/private", scratch / "spelled");
    Policy policy;
    policy.folders = {folder("kit", scratch / "kit"), folder("alt", scratch.path().string() + "/./kit/../kit/sub/.."),
                      folder("inside", scratch / "kit/private/deep"), folder("whole", scratch / "kit/private")};
    // A denied directory spelled through a link, a denied file, and a directory that does not exist yet.
    policy.deny = {(scratch / "spelled").string() + "/", (scratch / "kit/key.wav").string(),
                   (scratch / "kit/later").string()};
    const auto context = make(policy);
    void* c = context->handle();
    CHECK(readStatus(c, "kit/open.wav") == FMINUS_FILE_OK);
    CHECK(readStatus(c, "kit/sub/c.wav") == FMINUS_FILE_OK);
    CHECK(readStatus(c, "kit/private/a.wav") == FMINUS_FILE_REFUSED);
    CHECK(readStatus(c, "kit/private/deep/b.wav") == FMINUS_FILE_REFUSED);
    CHECK(readStatus(c, "kit/private/none.wav") == FMINUS_FILE_REFUSED);   // refused before it is looked for
    CHECK(readStatus(c, "alt/private/a.wav") == FMINUS_FILE_REFUSED);      // another folder, another spelling
    CHECK(readStatus(c, "alt/open.wav") == FMINUS_FILE_OK);
    CHECK(readStatus(c, "inside/b.wav") == FMINUS_FILE_REFUSED);           // a folder that lies in a denied one
    CHECK(readStatus(c, "whole/a.wav") == FMINUS_FILE_REFUSED);            // a folder that is one
    CHECK(readStatus(c, "kit/key.wav") == FMINUS_FILE_REFUSED);            // a denied file
    void* all = list(c, "kit/private/*.wav");
    CHECK(status(all) == FMINUS_FILE_REFUSED);
    fminus_rt_file_close(all);
    // Made after the policy: denied as soon as it exists.
    put(scratch / "kit/later/x.wav", "LATER");
    CHECK(readStatus(c, "kit/later/x.wav") == FMINUS_FILE_REFUSED);
    // Renamed after the policy: still denied under its new name (its identity was pinned when the context was made).
    fs::rename(scratch / "kit/private", scratch / "kit/renamed");
    CHECK(readStatus(c, "kit/renamed/a.wav") == FMINUS_FILE_REFUSED);
    // And moved out of the folder, then a new directory made under the old name: that one is denied too (afresh).
    put(scratch / "kit/private/n.wav", "NEW");
    fs::remove(scratch / "spelled");
    fs::create_directory_symlink(scratch / "kit/private", scratch / "spelled");
    CHECK(readStatus(c, "kit/private/n.wav") == FMINUS_FILE_REFUSED);
}

TEST_CASE("a denied path that cannot be looked up closes everything", "[runtime][files]")
{
    fixtures({"wav"});
    const Scratch scratch;
    put(scratch / "kit/a.wav", "a");
    put(scratch / "plain.wav", "a file, where a deny path walks through a directory");
    Policy policy;
    policy.folders = {folder("kit", scratch / "kit")};
    policy.deny = {(scratch / "plain.wav/x").string()};   // nothing can be there: missing, not an error
    CHECK(readStatus(make(policy)->handle(), "kit/a.wav") == FMINUS_FILE_OK);
    fs::create_symlink("loop2", scratch / "loop1");
    fs::create_symlink("loop1", scratch / "loop2");
    policy.deny = {(scratch / "loop1").string()};
    std::string error;
    CHECK(Context::create(policy, error) == nullptr);   // a loop: what it is cannot be told
    CHECK(error == "deny: '" + (scratch / "loop1").string() + "' cannot be opened");
}

TEST_CASE("a directory made at a denied path after the context stays denied wherever it goes", "[runtime][files]")
{
    // A deny path with nothing at it when the context is made has no identity to pin: what an operation finds there
    // is remembered for the context's life, so renaming the directory afterwards does not take it out of the deny-list.
    fixtures({"wav"});
    const Scratch scratch;
    put(scratch / "kit/open.wav", "open");
    Policy policy;
    policy.folders = {folder("kit", scratch / "kit")};
    policy.deny = {(scratch / "kit/late").string(), (scratch / "kit/late.wav").string()};
    const auto context = make(policy);
    void* c = context->handle();
    put(scratch / "kit/late/p.wav", "DENIED");
    put(scratch / "kit/late.wav", "DENIED");
    CHECK(readStatus(c, "kit/late/p.wav") == FMINUS_FILE_REFUSED);
    CHECK(readStatus(c, "kit/late.wav") == FMINUS_FILE_REFUSED);
    fs::rename(scratch / "kit/late", scratch / "kit/moved");
    fs::rename(scratch / "kit/late.wav", scratch / "kit/moved.wav");
    std::string got;
    CHECK(readStatus(c, "kit/moved/p.wav", &got) == FMINUS_FILE_REFUSED);
    CHECK(got.empty());
    CHECK(readStatus(c, "kit/moved.wav") == FMINUS_FILE_REFUSED);
    void* all = list(c, "kit/moved/*.wav");
    CHECK(status(all) == FMINUS_FILE_REFUSED);
    fminus_rt_file_close(all);
    // Into a directory of its own, and under a third name: still the denied directory.
    fs::create_directories(scratch / "kit/box");
    fs::rename(scratch / "kit/moved", scratch / "kit/box/third");
    CHECK(readStatus(c, "kit/box/third/p.wav") == FMINUS_FILE_REFUSED);
    CHECK(readStatus(c, "kit/open.wav") == FMINUS_FILE_OK);
    // A second directory made at the path is denied too, and so on.
    put(scratch / "kit/late/q.wav", "DENIED");
    CHECK(readStatus(c, "kit/late/q.wav") == FMINUS_FILE_REFUSED);
    fs::rename(scratch / "kit/late", scratch / "kit/fourth");
    CHECK(readStatus(c, "kit/fourth/q.wav") == FMINUS_FILE_REFUSED);
    // Another context knows only what it has seen itself.
    const auto other = make(policy);
    CHECK(readStatus(other->handle(), "kit/open.wav") == FMINUS_FILE_OK);
}

TEST_CASE("the operating system's refusal is FILE_DENIED", "[runtime][files]")
{
    fixtures({"wav"});
    const Scratch scratch;
    put(scratch / "kit/closed.wav", "closed");
    put(scratch / "kit/shut/a.wav", "shut");
    REQUIRE(::chmod((scratch / "kit/closed.wav").c_str(), 0) == 0);
    REQUIRE(::chmod((scratch / "kit/shut").c_str(), 0) == 0);
    Policy policy;
    policy.folders = {folder("kit", scratch / "kit")};
    const auto context = make(policy);
    if (::geteuid() != 0)
    {
        CHECK(readStatus(context->handle(), "kit/closed.wav") == FMINUS_FILE_DENIED);
        CHECK(readStatus(context->handle(), "kit/shut/a.wav") == FMINUS_FILE_DENIED);
        return;
    }
    // As root, permissions do not apply: a child that has given root up asks, through the context made before.
    const pid_t child = ::fork();
    REQUIRE(child >= 0);
    if (child == 0)
    {
        if (::setgid(65534) != 0 || ::setuid(65534) != 0)
            ::_exit(3);
        const bool closed = readStatus(context->handle(), "kit/closed.wav") == FMINUS_FILE_DENIED;
        const bool shut = readStatus(context->handle(), "kit/shut/a.wav") == FMINUS_FILE_DENIED;
        ::_exit(closed && shut ? 0 : 1);
    }
    int result = 0;
    ::waitpid(child, &result, 0);
    REQUIRE(WIFEXITED(result));
    if (WEXITSTATUS(result) == 3)
        WARN("the child could not give root up: FILE_DENIED is not checked");
    else
        CHECK(WEXITSTATUS(result) == 0);
}

TEST_CASE("listings: a snapshot of the regular files a program could name, sorted by their bytes",
          "[runtime][files]")
{
    fixtures({"f32", "wav"});
    const Scratch scratch;
    for (const char* name : {"b.f32", "A.F32", "c.f32", "pad1.f32", "PAD2.F32", "x.wav", "COM1.f32", "e f.f32"})
        put(scratch / "kit" / name, name);
    fs::create_directories(scratch / "kit/dir.f32");
    fs::create_symlink("b.f32", scratch / "kit/link.f32");
    REQUIRE(::mkfifo((scratch / "kit/pipe.f32").c_str(), 0644) == 0);
    put(scratch / "kit/.hidden.f32", "hidden");
    put(scratch / "kit/trail.f32 ", "trailing space");
    put(scratch / "kit/a:b.f32", "colon");
    put(scratch / "outside/h.f32", "SECRET");
    fs::create_hard_link(scratch / "outside/h.f32", scratch / "kit/hard.f32");
    put(scratch / "kit/sub/z.f32", "z");
    put(scratch / "kit/bank3/pad9.f32", "nine");
    Policy policy;
    policy.folders = {folder("kit", scratch / "kit")};
    const auto context = make(policy);
    void* c = context->handle();
    void* all = list(c, "kit/*.f32");
    REQUIRE(status(all) == FMINUS_FILE_OK);
    const std::vector<std::string> expected = {"A.F32", "PAD2.F32", "b.f32", "c.f32", "e f.f32", "hard.f32",
                                               "pad1.f32"};
    REQUIRE(fminus_rt_file_count(all) == static_cast<std::int32_t>(expected.size()));
    for (std::size_t i = 0; i < expected.size(); ++i)
    {
        void* file = fminus_rt_file_read_nth(all, static_cast<std::int32_t>(i));
        INFO(expected[i]);
        CHECK(status(file) == FMINUS_FILE_OK);
        if (expected[i] != "hard.f32")
            CHECK(bytesOf(file) == expected[i]);
        fminus_rt_file_close(file);
    }
    CHECK(status(fminus_rt_file_read_nth(all, -1)) == FMINUS_FILE_INVALID);
    CHECK(status(fminus_rt_file_read_nth(all, 7)) == FMINUS_FILE_INVALID);
    CHECK(fminus_rt_file_size(all) == 0);   // a listing is no file
    CHECK(fminus_rt_file_data(all) == nullptr);
    void* file = read(c, "kit/b.f32");
    CHECK(status(fminus_rt_file_read_nth(file, 0)) == FMINUS_FILE_INVALID);   // a file is no listing
    CHECK(fminus_rt_file_count(file) == 0);
    fminus_rt_file_close(file);

    // The folder changes after the snapshot: the listing means the same names.
    fs::remove(scratch / "kit/b.f32");
    put(scratch / "kit/0.f32", "new");
    put(scratch / "kit/c.f32", "replaced");
    CHECK(fminus_rt_file_count(all) == 7);
    CHECK(status(fminus_rt_file_read_nth(all, 2)) == FMINUS_FILE_MISSING);   // b.f32, gone
    file = fminus_rt_file_read_nth(all, 3);
    CHECK(bytesOf(file) == "replaced");   // c.f32, as it is now
    fminus_rt_file_close(file);
    fs::remove(scratch / "kit/pad1.f32");
    fs::create_symlink(scratch / "outside/h.f32", scratch / "kit/pad1.f32");
    CHECK(status(fminus_rt_file_read_nth(all, 6)) == FMINUS_FILE_REFUSED);   // a link now: checked again
    fminus_rt_file_close(all);

    // Patterns: the fixed parts without regard to letter case, a %d, a subdirectory; singleLink skips hard links.
    void* pads = list(c, "kit/pad*.f32");
    CHECK(fminus_rt_file_count(pads) == 1);   // PAD2.F32 (pad1.f32 is a link now)
    fminus_rt_file_close(pads);
    void* bank = list(c, "kit/bank%d/PAD*.F32", {3});
    REQUIRE(fminus_rt_file_count(bank) == 1);
    file = fminus_rt_file_read_nth(bank, 0);
    CHECK(bytesOf(file) == "nine");
    fminus_rt_file_close(file);
    fminus_rt_file_close(bank);
    void* sub = list(c, "kit/sub/*.f32");
    CHECK(fminus_rt_file_count(sub) == 1);
    fminus_rt_file_close(sub);
    void* none = list(c, "kit/missing/*.f32");
    CHECK(status(none) == FMINUS_FILE_MISSING);
    CHECK(fminus_rt_file_count(none) == 0);
    for (const char* pattern : {"kit/*", "kit/a.*", "kit/**.f32", "kit/*a*.f32", "kit/.*.f32", "kit/*.f%d",
                                "kit/*/a.f32", "kit/x.f32", "/kit/*.f32", "kit/*.", "kit/* .f32 "})
    {
        INFO(pattern);
        void* l = list(c, pattern);
        CHECK(status(l) == FMINUS_FILE_REFUSED);
        fminus_rt_file_close(l);
    }
    policy.singleLink = true;
    policy.maxListed = 3;
    const auto single = make(policy);
    void* few = list(single->handle(), "kit/*.f32");
    CHECK(status(few) == FMINUS_FILE_LIMIT);   // A.F32, PAD2.F32, c.f32, e f.f32, 0.f32: more than 3
    policy.maxListed = 5;
    const auto five = make(policy);
    few = list(five->handle(), "kit/*.f32");
    CHECK(status(few) == FMINUS_FILE_OK);
    CHECK(fminus_rt_file_count(few) == 5);   // hard.f32 skipped under singleLink
    fminus_rt_file_close(few);
}

TEST_CASE("every reason the policy's check refuses a policy", "[runtime][files]")
{
    const auto refused = [](const Policy& policy, const std::string& expected) {
        INFO(expected);
        CHECK(fminus::files::checkPolicy(policy) == expected);
        std::string error;
        CHECK(Context::create(policy, error) == nullptr);
        CHECK(error == expected);
    };
    Policy p;
    p.folders = {folder("", "/x")};
    refused(p, "folders: '' is not a folder's name: 1 to 64 ASCII letters, digits, '_' and '-', not beginning with "
               "'-'");
    for (const std::string& name : std::vector<std::string>{"-kit", "a/b", "a.b", "k it", std::string(65, 'k'),
                                                            "\xc3\xa9"})
    {
        p.folders = {folder(name, "/x")};
        refused(p, "folders: '" + name
                       + "' is not a folder's name: 1 to 64 ASCII letters, digits, '_' and '-', not beginning with "
                         "'-'");
    }
    p.folders = {folder("Kit", "/x"), folder("kit", "/y")};
    refused(p, "folders: 'kit' is named twice (letter case aside: 'Kit')");
    p.folders = {folder("kit", "")};
    refused(p, "folders: 'kit': its directory is empty");
    p.folders = {folder("kit", std::string("/a\0b", 4))};
    refused(p, "folders: 'kit': its directory has a NUL byte");
    p.folders = {folder("kit", "/x", false, {"w.av"})};
    refused(p, "folders: 'kit': types: 'w.av' is not an extension: 1 to 16 ASCII letters and digits, without the dot");
    p.folders = {folder("kit", "/x", false, {""})};
    refused(p, "folders: 'kit': types: '' is not an extension: 1 to 16 ASCII letters and digits, without the dot");
    p.folders = {};
    p.types = {"abcdefghijklmnopq"};
    refused(p, "types: 'abcdefghijklmnopq' is not an extension: 1 to 16 ASCII letters and digits, without the dot");
    p.types = {};
    p.deny = {""};
    refused(p, "deny: a path is empty");
    p.deny = {std::string("/a\0b", 4)};
    refused(p, "deny: '" + std::string("/a\0b", 4) + "' has a NUL byte");
    p.deny = {};
    p.maxFileBytes = 0;
    refused(p, "maxFileBytes is 0: from 1 to " + std::to_string(FMINUS_FILES_MAX_BYTES) + " (FMINUS_FILES_MAX_BYTES)");
    p.maxFileBytes = -5;
    refused(p, "maxFileBytes is -5: from 1 to " + std::to_string(FMINUS_FILES_MAX_BYTES) + " (FMINUS_FILES_MAX_BYTES)");
#if FMINUS_FILES_MAX_BYTES < 2147483647
    // The build's ceiling: no policy raises it.
    p.maxFileBytes = FMINUS_FILES_MAX_BYTES + 1;
    refused(p, "maxFileBytes is " + std::to_string(FMINUS_FILES_MAX_BYTES + 1) + ", over this build's ceiling of "
                   + std::to_string(FMINUS_FILES_MAX_BYTES) + " (FMINUS_FILES_MAX_BYTES)");
#endif
    p.maxFileBytes = FMINUS_FILES_MAX_BYTES;
    CHECK(fminus::files::checkPolicy(p).empty());
    CHECK(Policy{}.maxFileBytes <= FMINUS_FILES_MAX_BYTES);   // the default policy, valid in every build
    for (const int open : {0, -1, 65537})
    {
        p.maxOpen = open;
        refused(p, "maxOpen is " + std::to_string(open) + ": from 1 to 65536");
    }
    p.maxOpen = 65536;
    p.maxHeldBytes = -1;
    refused(p, "maxHeldBytes is -1: from 0 to 2^48");
    p.maxHeldBytes = (std::int64_t{1} << 48) + 1;
    refused(p, "maxHeldBytes is " + std::to_string((std::int64_t{1} << 48) + 1) + ": from 0 to 2^48");
    p.maxHeldBytes = 0;
    p.maxWrittenBytes = -1;
    refused(p, "maxWrittenBytes is -1: from 0 to 2^48");
    p.maxWrittenBytes = 0;
    p.maxFilesWritten = -1;
    refused(p, "maxFilesWritten is -1: from 0 to 2147483647");
    p.maxFilesWritten = 0;
    for (const int listed : {0, (1 << 20) + 1})
    {
        p.maxListed = listed;
        refused(p, "maxListed is " + std::to_string(listed) + ": from 1 to 1048576");
    }
    p.maxListed = 1;
    CHECK(fminus::files::checkPolicy(p).empty());

    // The build's ceilings.
    Policy w;
    w.folders = {folder("user", "/x", true)};
#if FMINUS_HAS_FILES_WRITE
    CHECK(fminus::files::checkPolicy(w).empty());
#else
    refused(w, "folders: 'user': write is set, and this build of FMinus leaves out writing files "
               "(FMINUS_FILES_WRITE=OFF)");
#endif
    Policy a;
    a.anywhere = true;
#if FMINUS_HAS_FILES_ANYWHERE
    CHECK(fminus::files::checkPolicy(a).empty());
#else
    refused(a, "anywhere is set, and this build of FMinus leaves out absolute names (FMINUS_FILES_ANYWHERE=OFF)");
#endif
    Policy t;
    t.types = {"wav", "zzzz9"};
    if (std::string_view(FMINUS_FILES_TYPES).empty())
        CHECK(fminus::files::checkPolicy(t).empty());
    else if (std::string_view(";" FMINUS_FILES_TYPES ";").find(";zzzz9;") == std::string_view::npos)
        CHECK(fminus::files::checkPolicy(t).find("'zzzz9' is not a type this build of FMinus can touch "
                                                 "(FMINUS_FILES_TYPES: ")
              != std::string::npos);
}

#if FMINUS_HAS_FILES_ANYWHERE
TEST_CASE("anywhere: absolute names, their directories' links followed, the deny-list on the real path",
          "[runtime][files]")
{
    fixtures({"txt"});
    const Scratch scratch;
    put(scratch / "real/a.txt", "real");
    put(scratch / "real/private/s.txt", "PRIVATE");
    fs::create_directory_symlink(scratch / "real", scratch / "via");
    fs::create_symlink("/dev/null", scratch / "null.txt");
    fs::create_symlink(scratch / "real/a.txt", scratch / "file.txt");
    Policy policy;
    policy.anywhere = true;
    policy.deny = {(scratch / "real/private").string()};
    const auto context = make(policy);
    void* c = context->handle();
    const std::string base = scratch.path().string();
    std::string got;
    CHECK(readStatus(c, base + "/real/a.txt", &got) == FMINUS_FILE_OK);
    CHECK(got == "real");
    CHECK(readStatus(c, base + "/via/a.txt", &got) == FMINUS_FILE_OK);   // a directory's link, followed
    CHECK(got == "real");
    CHECK(readStatus(c, base + "/via/private/s.txt") == FMINUS_FILE_REFUSED);   // denied on the real path
    CHECK(readStatus(c, base + "/real/private/s.txt") == FMINUS_FILE_REFUSED);
    CHECK(readStatus(c, base + "/file.txt") == FMINUS_FILE_REFUSED);   // the file itself is never a link
    CHECK(readStatus(c, base + "/null.txt") == FMINUS_FILE_REFUSED);
    CHECK(readStatus(c, "/dev/null") == FMINUS_FILE_REFUSED);
    CHECK(readStatus(c, "/proc/self/status") == FMINUS_FILE_REFUSED);
    CHECK(readStatus(c, base + "/../" + scratch.path().filename().string() + "/real/a.txt") == FMINUS_FILE_REFUSED);
    // A file of /proc that a program could name: a regular file to fstat, refused for what it is on (Linux).
    for (const char* proc : {"/proc/config.gz", "/proc/bus/pci/00/00.0"})
    {
        std::error_code error;
        if (fs::is_regular_file(proc, error))
        {
            INFO(proc);
            CHECK(readStatus(c, proc) == FMINUS_FILE_REFUSED);
        }
    }
    void* l = list(c, base + "/via/*.txt");
    REQUIRE(fminus_rt_file_count(l) == 1);
    void* f = fminus_rt_file_read_nth(l, 0);
    CHECK(bytesOf(f) == "real");
    fminus_rt_file_close(f);
    fminus_rt_file_close(l);
    // A policy without anywhere refuses every absolute name.
    Policy folders;
    folders.folders = {folder("real", scratch / "real")};
    CHECK(readStatus(make(folders)->handle(), base + "/real/a.txt") == FMINUS_FILE_REFUSED);
#if FMINUS_HAS_FILES_WRITE
    CHECK(write(c, base + "/real/w.txt", "x") == FMINUS_FILE_REFUSED);   // never written
    CHECK(append(c, base + "/real/w.txt", "x") == FMINUS_FILE_REFUSED);
    CHECK(!fs::exists(scratch / "real/w.txt"));
#endif
}
#endif

#if FMINUS_HAS_FILES_WRITE
TEST_CASE("file_write replaces a file whole or not at all", "[runtime][files]")
{
    fixtures({"f32"});
    const Scratch scratch;
    put(scratch / "user/take.f32", "old");
    put(scratch / "kit/read.f32", "read only");
    put(scratch / "outside/secret.f32", "SECRET");
    Policy policy;
    policy.folders = {folder("user", scratch / "user", true), folder("kit", scratch / "kit")};
    const auto context = make(policy);
    void* c = context->handle();
    CHECK(write(c, "user/new%d.f32", "fresh", {3}) == FMINUS_FILE_OK);
    CHECK(contents(scratch / "user/new3.f32") == "fresh");
    CHECK(write(c, "user/take.f32", "new contents") == FMINUS_FILE_OK);
    CHECK(contents(scratch / "user/take.f32") == "new contents");
    CHECK(write(c, "user/empty.f32", "") == FMINUS_FILE_OK);
    CHECK(fs::file_size(scratch / "user/empty.f32") == 0);
    // Each step failed in turn: the old file, whole, and no temporary file left.
    for (int step = 1; step <= 5; ++step)
    {
        INFO("step " << step);
        fminus::files::os::failNextReplaceAt(step);
        CHECK(write(c, "user/take.f32", "half written and lost") == FMINUS_FILE_FAILED);
        CHECK(contents(scratch / "user/take.f32") == "new contents");
        CHECK(!temporaryLeft(scratch / "user"));
    }
    fminus::files::os::failNextReplaceAt(6);   // the directory's flush, after the rename: the new file is in place
    CHECK(write(c, "user/take.f32", "renamed") == FMINUS_FILE_FAILED);
    CHECK(contents(scratch / "user/take.f32") == "renamed");
    CHECK(!temporaryLeft(scratch / "user"));
    // What is refused, and the target untouched.
    CHECK(write(c, "kit/read.f32", "x") == FMINUS_FILE_REFUSED);   // a folder not granted for writing
    CHECK(contents(scratch / "kit/read.f32") == "read only");
    CHECK(write(c, "user/x.f32/y.f32", "x") == FMINUS_FILE_MISSING);
    fs::create_symlink(scratch / "outside/secret.f32", scratch / "user/link.f32");
    CHECK(write(c, "user/link.f32", "x") == FMINUS_FILE_REFUSED);
    CHECK(fs::is_symlink(scratch / "user/link.f32"));
    CHECK(contents(scratch / "outside/secret.f32") == "SECRET");
    fs::create_directory_symlink(scratch / "outside", scratch / "user/door");
    CHECK(write(c, "user/door/secret.f32", "x") == FMINUS_FILE_REFUSED);
    CHECK(contents(scratch / "outside/secret.f32") == "SECRET");
    REQUIRE(::mkfifo((scratch / "user/pipe.f32").c_str(), 0644) == 0);
    CHECK(write(c, "user/pipe.f32", "x") == FMINUS_FILE_REFUSED);
    fs::create_directories(scratch / "user/dir.f32");
    CHECK(write(c, "user/dir.f32", "x") == FMINUS_FILE_REFUSED);
    CHECK(fs::is_directory(scratch / "user/dir.f32"));
    CHECK(write(c, "user/.hidden.f32", "x") == FMINUS_FILE_REFUSED);
    CHECK(fminus_rt_file_write(c, "user/n.f32", 10, nullptr, 0, "x", -1) == FMINUS_FILE_INVALID);
    CHECK(fminus_rt_file_write(c, "user/n.f32", 10, nullptr, 0, nullptr, 1) == FMINUS_FILE_INVALID);
    CHECK(!temporaryLeft(scratch / "user"));
}

TEST_CASE("file_append adds at the end, creating the file, never through a link", "[runtime][files]")
{
    fixtures({"log"});
    const Scratch scratch;
    put(scratch / "outside/secret.log", "SECRET");
    Policy policy;
    policy.folders = {folder("user", scratch / "user", true)};
    fs::create_directories(scratch / "user");
    policy.maxFileBytes = 10;
    const auto context = make(policy);
    void* c = context->handle();
    CHECK(append(c, "user/levels.log", "abc") == FMINUS_FILE_OK);
    CHECK(append(c, "user/levels.log", "def") == FMINUS_FILE_OK);
    CHECK(contents(scratch / "user/levels.log") == "abcdef");
    CHECK(append(c, "user/levels.log", "ghij") == FMINUS_FILE_OK);   // 10 bytes: the limit
    CHECK(append(c, "user/levels.log", "k") == FMINUS_FILE_LIMIT);
    CHECK(contents(scratch / "user/levels.log") == "abcdefghij");
    fs::create_hard_link(scratch / "outside/secret.log", scratch / "user/hard.log");
    CHECK(append(c, "user/hard.log", "x") == FMINUS_FILE_REFUSED);
    fs::create_symlink(scratch / "outside/secret.log", scratch / "user/link.log");
    CHECK(append(c, "user/link.log", "x") == FMINUS_FILE_REFUSED);
    REQUIRE(::mkfifo((scratch / "user/pipe.log").c_str(), 0644) == 0);
    CHECK(append(c, "user/pipe.log", "x") == FMINUS_FILE_REFUSED);
    CHECK(contents(scratch / "outside/secret.log") == "SECRET");
    CHECK(fminus_rt_file_append(c, "user/n.log", 10, nullptr, 0, "x", -1) == FMINUS_FILE_INVALID);
}

TEST_CASE("a write's temporary file left by a process that died is removed by a later write, once an hour old",
          "[runtime][files]")
{
    fixtures({"f32"});
    const Scratch scratch;
    put(scratch / "outside/keep.f32", "OUTSIDE");
    fs::create_directories(scratch / "user/sub");
    Policy policy;
    policy.folders = {folder("user", scratch / "user", true)};
    policy.maxFilesWritten = 1000000;
    policy.maxWrittenBytes = std::int64_t{1} << 40;
    // Processes killed while they write: what a write leaves when its process dies between its temporary file and the
    // rename (a crash, a power cut: the same).
    const std::string big(1 << 20, 'A');
    int left = 0;
    for (int kill = 0; kill < 60 && left == 0; ++kill)
    {
        const pid_t child = ::fork();
        REQUIRE(child >= 0);
        if (child == 0)
        {
            std::string error;
            const std::unique_ptr<Context> mine = Context::create(policy, error);
            for (;;)
                write(mine->handle(), "user/sub/take.f32", big);
        }
        ::usleep(static_cast<useconds_t>(2000 + kill * 500));
        ::kill(child, SIGKILL);
        int ignored = 0;
        ::waitpid(child, &ignored, 0);
        for (const fs::directory_entry& e : fs::directory_iterator(scratch / "user/sub"))
            left += e.path().filename().string().starts_with(".fminus-") ? 1 : 0;
    }
    INFO("temporary files left by killed writes: " << left);
    // Aged two hours, with others beside them: one of a minute ago (another process's, perhaps, still writing); a
    // link, a hard link and a directory of the same shape; names of another shape.
    const auto age = [](const fs::path& path)
    {
        const timespec old[2] = {{::time(nullptr) - 7200, 0}, {::time(nullptr) - 7200, 0}};
        ::utimensat(AT_FDCWD, path.c_str(), old, AT_SYMLINK_NOFOLLOW);
    };
    for (const fs::directory_entry& e : fs::directory_iterator(scratch / "user/sub"))
        age(e.path());
    put(scratch / "user/sub/.fminus-0123456789abcdef.tmp", "stale");
    age(scratch / "user/sub/.fminus-0123456789abcdef.tmp");
    put(scratch / "user/sub/.fminus-00000000000000aa.tmp", "fresh");
    fs::create_symlink(scratch / "outside/keep.f32", scratch / "user/sub/.fminus-00000000000000bb.tmp");
    age(scratch / "user/sub/.fminus-00000000000000bb.tmp");
    fs::create_hard_link(scratch / "outside/keep.f32", scratch / "user/sub/.fminus-00000000000000cc.tmp");
    age(scratch / "user/sub/.fminus-00000000000000cc.tmp");
    fs::create_directories(scratch / "user/sub/.fminus-00000000000000dd.tmp");
    age(scratch / "user/sub/.fminus-00000000000000dd.tmp");
    for (const char* other : {".fminus-0123456789ABCDEF.tmp", ".fminus-0123.tmp", ".fminus-0123456789abcdef.tmpx",
                              "fminus-0123456789abcdef.tmp", ".fminus-0123456789abcdeg.tmp"})
    {
        put(scratch / "user/sub" / other, "other");
        age(scratch / "user/sub" / other);
    }
    // A write elsewhere sweeps nothing here; the first in this directory does, and only the stale temporaries.
    const auto context = make(policy);
    CHECK(write(context->handle(), "user/top.f32", "top") == FMINUS_FILE_OK);
    CHECK(fs::exists(scratch / "user/sub/.fminus-0123456789abcdef.tmp"));
    CHECK(write(context->handle(), "user/sub/x.f32", "x") == FMINUS_FILE_OK);
    std::vector<std::string> remaining;
    for (const fs::directory_entry& e : fs::directory_iterator(scratch / "user/sub"))
        remaining.push_back(e.path().filename().string());
    std::ranges::sort(remaining);
    const std::vector<std::string> expected = {
        ".fminus-00000000000000aa.tmp", ".fminus-00000000000000bb.tmp", ".fminus-00000000000000cc.tmp",
        ".fminus-00000000000000dd.tmp", ".fminus-0123.tmp", ".fminus-0123456789ABCDEF.tmp",
        ".fminus-0123456789abcdef.tmpx", ".fminus-0123456789abcdeg.tmp", "fminus-0123456789abcdef.tmp", "x.f32"};
    std::vector<std::string> withTake = expected;
    withTake.push_back("take.f32");
    std::ranges::sort(withTake);
    CHECK((remaining == expected || remaining == withTake));
    CHECK(fs::is_symlink(scratch / "user/sub/.fminus-00000000000000bb.tmp"));
    CHECK(contents(scratch / "outside/keep.f32") == "OUTSIDE");
}

TEST_CASE("the write quotas count what a program attempts, for its context's life", "[runtime][files]")
{
    fixtures({"bin", "txt"});
    const Scratch scratch;
    fs::create_directories(scratch / "user");
    Policy policy;
    policy.folders = {folder("user", scratch / "user", true)};
    policy.maxWrittenBytes = 10;
    policy.maxFilesWritten = 3;
    policy.maxFileBytes = 8;
    const auto context = make(policy);
    void* c = context->handle();
    CHECK(write(c, "user/a.bin", "123456789") == FMINUS_FILE_LIMIT);   // over maxFileBytes: nothing counted
    CHECK(write(c, "user/a.bin", "1234") == FMINUS_FILE_OK);
    CHECK(write(c, "user/a.bin", "1234") == FMINUS_FILE_OK);
    CHECK(write(c, "user/b.bin", "123") == FMINUS_FILE_LIMIT);   // 11 bytes in all
    CHECK(write(c, "user/b.bin", "12") == FMINUS_FILE_OK);       // 10, and the third file
    CHECK(write(c, "user/c.bin", "") == FMINUS_FILE_LIMIT);      // a fourth
    CHECK(!fs::exists(scratch / "user/c.bin"));
    CHECK(fs::exists(scratch / "user/b.bin"));
    Policy appends = policy;
    appends.maxWrittenBytes = 100;
    appends.maxFilesWritten = 1;
    const auto second = make(appends);
    CHECK(append(second->handle(), "user/log.txt", "a") == FMINUS_FILE_OK);   // creates: the one file
    CHECK(append(second->handle(), "user/log.txt", "b") == FMINUS_FILE_OK);   // appends: no file made
    CHECK(append(second->handle(), "user/log2.txt", "c") == FMINUS_FILE_LIMIT);
    CHECK(!fs::exists(scratch / "user/log2.txt"));
    CHECK(contents(scratch / "user/log.txt") == "ab");
}

TEST_CASE("two programs writing one file: each write is whole, one or the other", "[runtime][files]")
{
    fixtures({"bin"});
    const Scratch scratch;
    fs::create_directories(scratch / "user");
    Policy policy;
    policy.folders = {folder("user", scratch / "user", true)};
    policy.maxFilesWritten = 100000;
    const auto first = make(policy);
    const auto second = make(policy);
    const std::string a(50000, 'a');
    const std::string b(70000, 'b');
    std::atomic<bool> stop{false};
    std::atomic<int> torn{0};
    std::thread reader([&] {
        while (!stop.load())
        {
            const std::string now = contents(scratch / "user/shared.bin");
            if (!now.empty() && now != a && now != b)
                ++torn;
        }
    });
    // Counted, and checked once the threads are done: Catch2's assertions are for one thread at a time.
    std::atomic<int> failed{0};
    std::thread one([&] {
        for (int i = 0; i < 200; ++i)
            failed += write(first->handle(), "user/shared.bin", a) == FMINUS_FILE_OK ? 0 : 1;
    });
    std::thread two([&] {
        for (int i = 0; i < 200; ++i)
            failed += write(second->handle(), "user/shared.bin", b) == FMINUS_FILE_OK ? 0 : 1;
    });
    one.join();
    two.join();
    stop = true;
    reader.join();
    CHECK(failed == 0);
    CHECK(torn == 0);
    const std::string last = contents(scratch / "user/shared.bin");
    CHECK((last == a || last == b));
    CHECK(!temporaryLeft(scratch / "user"));
}
#endif

#if defined(__linux__)
TEST_CASE("a directory or a file swapped for a link while it is opened never leads out of the folder",
          "[runtime][files]")
{
    fixtures({"txt"});
    // Someone who can write the folder exchanges a directory, and a file, with links to outside, over and over, while
    // a program reads through them: the walk opens each segment beneath the last without following a link, so a read
    // gives the inside file or is refused, never the outside one.
    const Scratch scratch;
    put(scratch / "kit/sub/a.txt", "inside");
    put(scratch / "kit/f.txt", "inside");
    put(scratch / "outside/a.txt", "SECRET");
    put(scratch / "outside/f.txt", "SECRET");
    fs::create_directory_symlink(scratch / "outside", scratch / "kit/dirlink");
    fs::create_symlink(scratch / "outside/f.txt", scratch / "kit/filelink");
    Policy policy;
    policy.folders = {folder("kit", scratch / "kit")};
    const auto context = make(policy);
    std::atomic<bool> stop{false};
    std::atomic<long> swaps{0};
    std::thread swapper([&] {
        const std::string sub = (scratch / "kit/sub").string();
        const std::string dirlink = (scratch / "kit/dirlink").string();
        const std::string f = (scratch / "kit/f.txt").string();
        const std::string filelink = (scratch / "kit/filelink").string();
        while (!stop.load())
        {
            ::syscall(SYS_renameat2, AT_FDCWD, sub.c_str(), AT_FDCWD, dirlink.c_str(), 2 /* RENAME_EXCHANGE */);
            ::syscall(SYS_renameat2, AT_FDCWD, f.c_str(), AT_FDCWD, filelink.c_str(), 2);
            ++swaps;
        }
    });
    long leaked = 0;
    long inside = 0;
    for (int i = 0; i < 4000; ++i)
    {
        for (const char* name : {"kit/sub/a.txt", "kit/f.txt"})
        {
            std::string got;
            const std::int32_t s = readStatus(context->handle(), name, &got);
            if (got == "SECRET")
                ++leaked;
            if (s == FMINUS_FILE_OK && got == "inside")
                ++inside;
        }
    }
    stop = true;
    swapper.join();
    INFO("swaps " << swaps.load() << ", reads of the inside file " << inside);
    CHECK(swaps.load() > 0);
    CHECK(leaked == 0);
}
#endif

#if defined(__linux__)
TEST_CASE("a directory swapped in and out of a denied path while names are walked is never read", "[runtime][files]")
{
    // A deny path with nothing at it when the context was made: a directory made there afterwards, seen there once, is
    // exchanged with a public one over and over, while a program reads through both names. What is at a denied path is
    // looked at again once the walk is done and once the file is read, and an operation during which it changed is
    // refused, so the denied bytes are never returned, through either name.
    fixtures({"wav"});
    const Scratch scratch;
    put(scratch / "kit/pub/p.wav", "public");
    Policy policy;
    policy.folders = {folder("kit", scratch / "kit")};
    policy.deny = {(scratch / "kit/late").string()};
    const auto context = make(policy);
    put(scratch / "kit/late/p.wav", "DENIED");
    CHECK(readStatus(context->handle(), "kit/late/p.wav") == FMINUS_FILE_REFUSED);   // seen there
    std::atomic<bool> stop{false};
    std::atomic<long> swaps{0};
    std::thread swapper([&] {
        const std::string late = (scratch / "kit/late").string();
        const std::string pub = (scratch / "kit/pub").string();
        while (!stop.load())
        {
            ::syscall(SYS_renameat2, AT_FDCWD, late.c_str(), AT_FDCWD, pub.c_str(), 2 /* RENAME_EXCHANGE */);
            ++swaps;
        }
    });
    long leaked = 0;
    for (int i = 0; i < 4000; ++i)
        for (const char* name : {"kit/late/p.wav", "kit/pub/p.wav"})
        {
            std::string got;
            readStatus(context->handle(), name, &got);
            if (got == "DENIED")
                ++leaked;
            void* all = list(context->handle(), "kit/pub/*.wav");
            if (fminus_rt_file_count(all) == 1)
            {
                void* nth = fminus_rt_file_read_nth(all, 0);
                if (bytesOf(nth) == "DENIED")
                    ++leaked;
                fminus_rt_file_close(nth);
            }
            fminus_rt_file_close(all);
        }
    stop = true;
    swapper.join();
    INFO("swaps " << swaps.load());
    CHECK(swaps.load() > 0);
    CHECK(leaked == 0);
    CHECK(context->held() == 0);
}
#endif

TEST_CASE("the reads of a file and a listing allocate nothing and make no system call", "[runtime][files]")
{
    fixtures({"f32"});
    using fminus::test::HeapCalls;
    const Scratch scratch;
    put(scratch / "kit/a.f32", "abcd");
    put(scratch / "kit/b.f32", "efghij");
    Policy policy;
    policy.folders = {folder("kit", scratch / "kit")};
    const auto context = make(policy);
    void* a = read(context->handle(), "kit/a.f32");
    void* l = list(context->handle(), "kit/*.f32");
    void* missing = read(context->handle(), "kit/none.f32");
    void* refused = list(nullptr, "kit/*.f32");
    REQUIRE(status(a) == FMINUS_FILE_OK);
    REQUIRE(fminus_rt_file_count(l) == 2);
    const auto readAll = [&] {
        long sum = 0;
        for (const void* h : {a, l, missing, refused, static_cast<void*>(nullptr)})
        {
            sum += fminus_rt_file_status(h) + fminus_rt_file_size(h) + fminus_rt_file_count(h);
            const void* data = fminus_rt_file_data(h);
            sum += data != nullptr ? static_cast<const unsigned char*>(data)[0] : 0;
        }
        return sum;
    };
    const long expected = readAll();
    if (HeapCalls::counted())
    {
        long calls = 0;
        bool same = true;
        {
            const HeapCalls counting;
            for (int i = 0; i < 1000; ++i)
                same = same && readAll() == expected;
            calls = counting.count();
        }
        CHECK(same);
        CHECK(calls == 0);
        long reading = 0;
        {
            const HeapCalls counting;
            fminus_rt_file_close(read(context->handle(), "kit/b.f32"));
            reading = counting.count();
        }
        CHECK(reading >= 2);   // the counter sees the runtime's heap: the bytes and the handle, and their frees
    }
#if defined(__linux__)
    // In a child under seccomp's strict mode, where any system call but read, write and exit kills it.
    int fds[2];
    REQUIRE(::pipe(fds) == 0);
    const pid_t child = ::fork();
    REQUIRE(child >= 0);
    if (child == 0)
    {
        ::close(fds[0]);
        if (::prctl(PR_SET_SECCOMP, SECCOMP_MODE_STRICT) != 0)
            ::syscall(SYS_exit, 2);
        const long sum = readAll();
        const ssize_t written = ::write(fds[1], &sum, sizeof sum);
        ::syscall(SYS_exit, written == sizeof sum ? 0 : 1);
    }
    ::close(fds[1]);
    int result = 0;
    ::waitpid(child, &result, 0);
    long sum = 0;
    const ssize_t got = ::read(fds[0], &sum, sizeof sum);
    ::close(fds[0]);
    INFO("status " << result);
    if (WIFEXITED(result) && WEXITSTATUS(result) == 2)
        WARN("seccomp's strict mode is not available here: the system calls are not checked");
    else
    {
        CHECK(WIFEXITED(result));
        CHECK(WEXITSTATUS(result) == 0);
        CHECK(got == static_cast<ssize_t>(sizeof sum));
        CHECK(sum == expected);
    }
#endif
    fminus_rt_file_close(a);
    fminus_rt_file_close(l);
}

TEST_CASE("an executable's context comes from its environment, and none by default", "[runtime][files]")
{
    fixtures({"wav"});
    const Scratch scratch;
    put(scratch / "kit/a.wav", "from the environment");
    put(scratch / "kit/private/b.wav", "PRIVATE");
    const char* names[] = {"FMINUS_FILES_READ", "FMINUS_FILES_WRITE", "FMINUS_FILES_DENY", "FMINUS_FILES_TYPES",
                           "FMINUS_FILES_ANYWHERE"};
    struct Restore
    {
        std::vector<std::pair<std::string, std::string>> saved;
        std::vector<std::string> unset;
        ~Restore()
        {
            for (const auto& [name, value] : saved)
                ::setenv(name.c_str(), value.c_str(), 1);
            for (const std::string& name : unset)
                ::unsetenv(name.c_str());
        }
    } restore;
    for (const char* name : names)
    {
        if (const char* value = std::getenv(name))
            restore.saved.emplace_back(name, value);
        else
            restore.unset.emplace_back(name);
        ::unsetenv(name);
    }
    void* context = reinterpret_cast<void*>(1);
    CHECK(fminus_rt_files_main_start(&context, "prog") == 0);
    CHECK(context == nullptr);   // no files
    ::setenv("FMINUS_FILES_DENY", "/", 1);
    ::setenv("FMINUS_FILES_TYPES", "wav", 1);
    CHECK(fminus_rt_files_main_start(&context, "prog") == 0);
    CHECK(context == nullptr);   // still none: a deny-list and types grant nothing
    ::setenv("FMINUS_FILES_READ", ("kit=" + (scratch / "kit").string()).c_str(), 1);
    ::setenv("FMINUS_FILES_DENY", (scratch / "kit/private").string().c_str(), 1);
    REQUIRE(fminus_rt_files_main_start(&context, "prog") == 0);
    REQUIRE(context != nullptr);
    std::string got;
    CHECK(readStatus(context, "kit/a.wav", &got) == FMINUS_FILE_OK);
    CHECK(got == "from the environment");
    CHECK(readStatus(context, "kit/private/b.wav") == FMINUS_FILE_REFUSED);
    fminus_rt_files_main_stop(context);
    fminus_rt_files_main_stop(nullptr);
    ::setenv("FMINUS_FILES_READ", "kit", 1);
    CHECK(fminus_rt_files_main_start(&context, "prog") == 2);   // not name=directory
    CHECK(context == nullptr);
    ::setenv("FMINUS_FILES_READ", ("-kit=" + (scratch / "kit").string()).c_str(), 1);
    CHECK(fminus_rt_files_main_start(&context, "prog") == 2);
    ::setenv("FMINUS_FILES_READ", ("kit=" + (scratch / "kit").string()).c_str(), 1);
    ::setenv("FMINUS_FILES_TYPES", "w.av", 1);
    CHECK(fminus_rt_files_main_start(&context, "prog") == 2);
    ::setenv("FMINUS_FILES_TYPES", "wav", 1);
    ::setenv("FMINUS_FILES_ANYWHERE", "yes", 1);
    CHECK(fminus_rt_files_main_start(&context, "prog") == 2);
    ::setenv("FMINUS_FILES_ANYWHERE", "1", 1);
    CHECK(fminus_rt_files_main_start(&context, "prog") == (FMINUS_HAS_FILES_ANYWHERE ? 0 : 2));
    fminus_rt_files_main_stop(context);
    ::unsetenv("FMINUS_FILES_ANYWHERE");
    ::setenv("FMINUS_FILES_WRITE", ("user=" + (scratch / "kit").string()).c_str(), 1);
    CHECK(fminus_rt_files_main_start(&context, "prog") == (FMINUS_HAS_FILES_WRITE ? 0 : 2));
    fminus_rt_files_main_stop(context);
}
#endif
