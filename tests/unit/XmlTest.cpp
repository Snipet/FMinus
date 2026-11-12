// tests/unit/XmlTest.cpp: the XML parser and its builtins (extending.md, recipe 40), made as the JSON parser's are
// (JsonTest.cpp): a well-formed document and not one that is not, within the size and depth limits; what a document
// holds besides its elements (a DOCTYPE and its internal subset, CDATA, comments, processing instructions, references,
// prefixed names); the entities a document declares never expanded and an external one never fetched; each read on
// each kind of node, its fallback when the value is not of its kind; a float correctly rounded from its digits in any
// locale; a path prepared when the program is compiled (a small part of XPath, its indices from 0, with places for
// ints), its errors at their columns; the effects, parsing off the audio thread and every read on it, and the
// runtime's promise behind them: a read allocates nothing and makes no system call. The runtime's functions are called
// directly, with paths prepared by the compiler's own scanXmlPath, and through programs the JIT runs. In a build
// without FMINUS_XML, every xml_ builtin is the error that names the option.
#include "Compile.h"
#include "HeapCalls.h"
#include "fminus/Config.h"
#include "ir/Lowering.h"
#include "runtime/Runtime.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <bit>
#include <clocale>
#include <csignal>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#if defined(__linux__)
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace fminus::test;

namespace
{
    // Every xml_ builtin, and a call of each a program could make.
    constexpr std::string_view kCalls[][2] = {
        {"xml_parse", "void* a(const void* d, int n) { return xml_parse(d, n); }\n"},
        {"xml_free", "void b(void* d) { xml_free(d); }\n"},
        {"xml_float", "float c(const void* d) { return xml_float(d, \"/a\", 0.0f); }\n"},
        {"xml_double", "double e(const void* d) { return xml_double(d, \"/a\", 0.0); }\n"},
        {"xml_int", "int f(const void* d) { return xml_int(d, \"/a/@b\", 0); }\n"},
        {"xml_bool", "bool g(const void* d) { return xml_bool(d, \"/a\", false); }\n"},
        {"xml_count", "int h(const void* d) { return xml_count(d, \"/a/b\"); }\n"},
        {"xml_floats", "int i(const void* d, float* o) { return xml_floats(d, \"/a\", o, 4); }\n"},
        {"xml_choice", "int j(const void* d) { return xml_choice(d, \"/a\", \"x\", \"y\"); }\n"},
    };
} // namespace

#if FMINUS_HAS_XML
namespace
{
    // A document the runtime parsed (null when it is no XML), freed at the end.
    struct Document
    {
        void* doc;

        explicit Document(std::string_view text)
            : doc(fminus_rt_xml_parse(text.data(), static_cast<std::int32_t>(text.size())))
        {
        }
        ~Document()
        {
            fminus_rt_xml_free(doc);
        }
        Document(const Document&) = delete;
        Document& operator=(const Document&) = delete;
    };

    bool parses(std::string_view text)
    {
        return Document(text).doc != nullptr;
    }

    // An XML path prepared as the compiler prepares one (ir/Paths.cpp, scanXmlPath), laid out as the program's
    // constant is (runtime/Runtime.h): its counts, its code, then its bytes.
    std::vector<std::int32_t> prepared(std::string_view path)
    {
        const fminus::ir::detail::PathScan scan = fminus::ir::detail::scanXmlPath(path);
        INFO(path);
        REQUIRE(scan.errors.empty());
        std::vector<std::int32_t> table{static_cast<std::int32_t>(scan.code.size()),
                                        static_cast<std::int32_t>(scan.bytes.size())};
        table.insert(table.end(), scan.code.begin(), scan.code.end());
        const std::size_t at = table.size();
        table.resize(at + (scan.bytes.size() + 3) / 4);
        std::memcpy(table.data() + at, scan.bytes.data(), scan.bytes.size());
        return table;
    }

    using Ints = std::vector<std::int32_t>;

    // The reads, through the runtime's functions, at a path with its ints.
    float xmlFloat(const Document& d, std::string_view path, float fallback, const Ints& ints = {})
    {
        return fminus_rt_xml_float(d.doc, prepared(path).data(), ints.data(), static_cast<std::int32_t>(ints.size()),
                                   fallback);
    }

    double xmlDouble(const Document& d, std::string_view path, double fallback)
    {
        return fminus_rt_xml_double(d.doc, prepared(path).data(), nullptr, 0, fallback);
    }

    std::int32_t xmlInt(const Document& d, std::string_view path, std::int32_t fallback, const Ints& ints = {})
    {
        return fminus_rt_xml_int(d.doc, prepared(path).data(), ints.data(), static_cast<std::int32_t>(ints.size()),
                                 fallback);
    }

    std::int32_t xmlBool(const Document& d, std::string_view path, std::int32_t fallback)
    {
        return fminus_rt_xml_bool(d.doc, prepared(path).data(), nullptr, 0, fallback);
    }

    std::int32_t xmlCount(const Document& d, std::string_view path, const Ints& ints = {})
    {
        return fminus_rt_xml_count(d.doc, prepared(path).data(), ints.data(), static_cast<std::int32_t>(ints.size()));
    }

    std::int32_t xmlFloats(const Document& d, std::string_view path, float* out, std::int32_t cap)
    {
        return fminus_rt_xml_floats(d.doc, prepared(path).data(), nullptr, 0, out, cap);
    }

    std::int32_t xmlChoice(const Document& d, std::string_view path, const std::vector<std::string_view>& choices)
    {
        std::vector<std::int32_t> table{static_cast<std::int32_t>(choices.size()), 0};
        std::string bytes;
        for (std::string_view choice : choices)
        {
            table.push_back(static_cast<std::int32_t>(choice.size()));
            bytes += choice;
        }
        table[1] = static_cast<std::int32_t>(bytes.size());
        const std::size_t at = table.size();
        table.resize(at + (bytes.size() + 3) / 4);
        std::memcpy(table.data() + at, bytes.data(), bytes.size());
        return fminus_rt_xml_choice(d.doc, prepared(path).data(), nullptr, 0, table.data());
    }

    std::string nested(int depth, std::string_view open, std::string_view inner, std::string_view close)
    {
        std::string text;
        for (int i = 0; i < depth; ++i)
            text += open;
        text += inner;
        for (int i = 0; i < depth; ++i)
            text += close;
        return text;
    }

    bool sameBits(float a, float b)
    {
        return std::bit_cast<std::uint32_t>(a) == std::bit_cast<std::uint32_t>(b);
    }

    // A preset, as a host would hand one to a program, and the program's reads of it: the same as JsonTest.cpp's,
    // written as XML writes it, with a DOCTYPE, a comment, CDATA, a character reference and a prefixed name.
    constexpr std::string_view kPreset = R"(<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE preset [<!ENTITY name "never expanded">]>
<!-- a preset -->
<preset version="2">
    <filter cutoff="2500.5" q=" 0.7071 "/>
    <gain>0.1</gain>
    <master voices="6"><tuning>440.0</tuning></master>
    <lfo sync="true"/><lfo sync="0"/>
    <osc wave="saw"><detune>0.25</detune></osc>
    <osc wave="square"><detune>-3</detune></osc>
    <osc wave="tri"><detune><![CDATA[1e-3]]></detune></osc>
    <table>1 2 3</table>
    <table>4.5, 5.5</table>
    <osc_1_level>0.75</osc_1_level>
    <fm:depth xmlns:fm="urn:fminus">&#52;2</fm:depth>
</preset>)";

    constexpr std::string_view kReads = R"(
        void* parse(const void* data, int bytes) { return xml_parse(data, bytes); }
        void release(void* doc) { xml_free(doc); }
        float cutoff(const void* doc) [[clang::nonblocking]]
        {
            return xml_float(doc, "/preset/filter/@cutoff", 1000.0f);
        }
        double gain(const void* doc) [[clang::nonblocking]] { return xml_double(doc, "/preset/gain", -1.0); }
        int voices(const void* doc) [[clang::nonblocking]] { return xml_int(doc, "/preset/master/@voices", 8); }
        bool sync(const void* doc, int k) [[clang::nonblocking]]
        {
            return xml_bool(doc, "/preset/lfo[%d]/@sync", k, true);
        }
        int oscillators(const void* doc) [[clang::nonblocking]] { return xml_count(doc, "/preset/osc"); }
        float detune(const void* doc, int k) [[clang::nonblocking]]
        {
            return xml_float(doc, "/preset/osc[%d]/detune", k, 100.0f);
        }
        enum Wave { Sine, Saw, Square };
        int wave(const void* doc, int k) [[clang::nonblocking]]
        {
            return xml_choice(doc, "/preset/osc[%d]/@wave", k, "sine", "saw", "square");
        }
        int table(const void* doc, int t, float* row, int cap) [[clang::nonblocking]]
        {
            return xml_floats(doc, "/preset/table[%d]", t, row, cap);
        }
        float level(const void* doc, int k) [[clang::nonblocking]]
        {
            return xml_float(doc, "/preset/osc_%d_level", k, 9.0f);
        }
        int prefixed(const void* doc) [[clang::nonblocking]] { return xml_int(doc, "/preset/fm:depth", 0); }
        int version(const void* doc) [[clang::nonblocking]] { return xml_int(doc, "/preset[0]/@version", 0); }
        bool mode(const void* doc, enum Wave w) [[clang::nonblocking]]
        {
            return xml_bool(doc, "/preset/lfo[%d]/@sync", w, 0);
        }
    )";

    struct Reads
    {
        std::unique_ptr<fminus::jit::Jit> jit;
        void* (*parse)(const void*, int) = nullptr;
        void (*release)(void*) = nullptr;
        float (*cutoff)(const void*) = nullptr;
        double (*gain)(const void*) = nullptr;
        int (*voices)(const void*) = nullptr;
        bool (*sync)(const void*, int) = nullptr;
        int (*oscillators)(const void*) = nullptr;
        float (*detune)(const void*, int) = nullptr;
        int (*wave)(const void*, int) = nullptr;
        int (*table)(const void*, int, float*, int) = nullptr;
        float (*level)(const void*, int) = nullptr;
        int (*prefixed)(const void*) = nullptr;
        int (*version)(const void*) = nullptr;
        bool (*mode)(const void*, int) = nullptr;

        Reads()
        {
            std::string errors;
            jit = compile(kReads, &errors);
            INFO(errors);
            REQUIRE(jit);
            parse = lookup<void*(const void*, int)>(*jit, "parse");
            release = lookup<void(void*)>(*jit, "release");
            cutoff = lookup<float(const void*)>(*jit, "cutoff");
            gain = lookup<double(const void*)>(*jit, "gain");
            voices = lookup<int(const void*)>(*jit, "voices");
            sync = lookup<bool(const void*, int)>(*jit, "sync");
            oscillators = lookup<int(const void*)>(*jit, "oscillators");
            detune = lookup<float(const void*, int)>(*jit, "detune");
            wave = lookup<int(const void*, int)>(*jit, "wave");
            table = lookup<int(const void*, int, float*, int)>(*jit, "table");
            level = lookup<float(const void*, int)>(*jit, "level");
            prefixed = lookup<int(const void*)>(*jit, "prefixed");
            version = lookup<int(const void*)>(*jit, "version");
            mode = lookup<bool(const void*, int)>(*jit, "mode");
        }
    };

#if defined(__linux__)
    // Runs `body` in a child process that a seccomp filter kills if it opens a file, makes a socket, connects one, or
    // runs a program: its exit status (0 when body returned true), 2 when the filter could not be installed, or the
    // signal that killed it, negated.
    template <typename Body>
    int confined(Body&& body)
    {
        std::vector<sock_filter> filter{BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, nr))};
        for (const long call : {
#if defined(SYS_open)
                 static_cast<long>(SYS_open),
#endif
#if defined(SYS_creat)
                 static_cast<long>(SYS_creat),
#endif
                 static_cast<long>(SYS_openat), static_cast<long>(SYS_openat2), static_cast<long>(SYS_socket),
                 static_cast<long>(SYS_connect), static_cast<long>(SYS_execve)})
        {
            filter.push_back(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, static_cast<std::uint32_t>(call), 0, 1));
            filter.push_back(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS));
        }
        filter.push_back(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW));
        const sock_fprog program{static_cast<unsigned short>(filter.size()), filter.data()};
        const pid_t child = ::fork();
        REQUIRE(child >= 0);
        if (child == 0)
        {
            if (::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0 || ::prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program))
                ::_exit(2);
            ::_exit(body() ? 0 : 1);
        }
        int status = 0;
        ::waitpid(child, &status, 0);
        return WIFEXITED(status) ? WEXITSTATUS(status) : -WTERMSIG(status);
    }
#endif
} // namespace

TEST_CASE("xml_parse takes one well-formed XML document, and refuses what is not one", "[runtime][xml]")
{
    // Well formed: a root element, alone or after a declaration, a DOCTYPE (with an internal subset, ']' and '>' in
    // its strings), comments and processing instructions, a byte order mark; whatever references and CDATA hold.
    for (std::string_view valid : {
             "<a/>", "<a></a>", " \n<a/>\n ", "<?xml version=\"1.0\" encoding=\"UTF-8\"?><a/>", "\xef\xbb\xbf<a/>",
             "<!DOCTYPE a><a/>", "<!DOCTYPE a SYSTEM \"a.dtd\"><a/>",
             "<!DOCTYPE a [<!ELEMENT a (#PCDATA)><!ATTLIST a b CDATA \"1\"><!ENTITY e \"]>\">]><a/>",
             "<!-- c --><?pi data?><a><!-- d --><?q?></a><!-- e -->", "<a b='1' c=\"2\"/>",
             "<a>&lt;&gt;&amp;&apos;&quot;</a>",
             "<a>&#65;&#x42;&#x1F600;</a>", "<a><![CDATA[<not> &markup;]]></a>", "<p:a xmlns:p=\"urn:p\"><p:b/></p:a>",
             "<a>\xe2\x82\xac</a>", "<a\n  b = '1'\n/>",
         })
    {
        INFO(valid);
        CHECK(parses(valid));
    }
    // Not: nothing, no root element, two of them, anything unclosed or mismatched, an attribute without quotes, a name
    // that cannot start one, markup after the root element.
    for (std::string_view invalid : {
             "", " ", "\n", "text", "<?xml version=\"1.0\"?>", "<!-- c -->", "<!DOCTYPE a>", "<a/><b/>",
             "<a></a><b></b>", "<a>", "<a><b></a>", "<a></b>", "<a>1</a></a>", "<a b='1'", "<a b=1/>",
             "<a b='1'c='2'/>",
             "<1a/>", "<a/><", "<a/>junk<", "<a><!-- c</a>", "<a><![CDATA[x</a>", "<!DOCTYPE a [<a/>", "<a b='x>",
         })
    {
        INFO(invalid);
        CHECK(!parses(invalid));
    }
    CHECK(!parses(std::string_view("\0", 1)));
    // No data, or a size that is not positive, is no document.
    CHECK(fminus_rt_xml_parse(nullptr, 4) == nullptr);
    CHECK(fminus_rt_xml_parse("<a/>", 0) == nullptr);
    CHECK(fminus_rt_xml_parse("<a/>", -4) == nullptr);
    // The document holds what it needs: the text may change, or go, once it is parsed.
    std::string text = "<a b=\"1.5\"><c>saw</c></a>";
    const Document d(text);
    REQUIRE(d.doc != nullptr);
    std::fill(text.begin(), text.end(), 'x');
    CHECK(xmlFloat(d, "/a/@b", 0.0f) == 1.5f);
    CHECK(xmlChoice(d, "/a/c", {"sine", "saw"}) == 1);
    fminus_rt_xml_free(nullptr);   // nothing
}

TEST_CASE("xml_parse refuses a document nested past its depth or larger than its size", "[runtime][xml]")
{
    constexpr int kDepth = FMINUS_XML_MAX_DEPTH;
    CHECK(parses(nested(kDepth, "<a>", "", "</a>")));
    CHECK(!parses(nested(kDepth + 1, "<a>", "", "</a>")));
    CHECK(parses(nested(kDepth, "<a x='1'>", "<!-- c -->text", "</a>")));   // only elements count
    CHECK(!parses(nested(kDepth, "<a>", "<b/>", "</a>")));                 // an empty element is one
    // Far past it, opened and never closed, or closed: refused, and without recursion (no stack to run out of).
    CHECK(!parses(nested(1000000, "<a>", "", "")));
    CHECK(!parses(nested(200000, "<a>", "", "</a>")));
    // At the depth, every level is there to read.
    const Document deep(nested(kDepth, "<a>", "7", "</a>"));
    REQUIRE(deep.doc != nullptr);
    std::string path;
    for (int i = 0; i < kDepth; ++i)
        path += "/a";
    CHECK(xmlInt(deep, path, -1) == 7);
    CHECK(xmlCount(deep, path) == 1);

    // The size: FMINUS_XML_MAX_BYTES at most, whatever the bytes are.
    std::string large = "<a>" + std::string(FMINUS_XML_MAX_BYTES - 7, ' ') + "</a>";
    REQUIRE(large.size() == FMINUS_XML_MAX_BYTES);
    CHECK(parses(large));
    large.insert(3, " ");
    CHECK(!parses(large));
}

TEST_CASE("entities are inert: a declared one is never expanded, an external one never fetched", "[runtime][xml]")
{
    // A file whose contents an external entity names: what a parser that fetches would put in the text.
    const std::filesystem::path secret = std::filesystem::temp_directory_path() / "fminus-xml-test-secret.txt";
    {
        std::FILE* file = std::fopen(secret.string().c_str(), "wb");
        REQUIRE(file != nullptr);
        std::fputs("FMINUS-SECRET", file);
        std::fclose(file);
    }
    const std::string external = "<!DOCTYPE a [<!ENTITY secret SYSTEM \"file://" + secret.string()
                                 + "\"><!ENTITY remote SYSTEM \"http://127.0.0.1:9/x\">]>\n"
                                   "<a b=\"&secret;\"><c>&secret;</c><d>&remote;</d></a>";
    const std::string parameter = "<!DOCTYPE a [<!ENTITY % p SYSTEM \"http://127.0.0.1:9/p.dtd\"> %p;]><a>&p;</a>";
    const std::string internal = "<!DOCTYPE a [<!ENTITY name \"expanded\">]><a n=\"&name;\">&name;</a>";
    // Billion laughs: lol9 is 10^9 lols, about 3 GB, to a parser that expands it.
    std::string laughs = "<?xml version=\"1.0\"?>\n<!DOCTYPE lolz [\n<!ENTITY lol \"lol\">\n";
    for (int i = 1; i <= 9; ++i)
    {
        laughs += "<!ENTITY lol" + std::to_string(i) + " \"";
        for (int k = 0; k < 10; ++k)
            laughs += i == 1 ? "&lol;" : "&lol" + std::to_string(i - 1) + ";";
        laughs += "\">\n";
    }
    laughs += "]>\n<lolz a=\"&lol9;\">&lol9;</lolz>";

    // Each parses, and holds each reference as it was written.
    const auto inert = [&] {
        const Document e(external);
        const Document p(parameter);
        const Document i(internal);
        const Document l(laughs);
        return e.doc != nullptr && p.doc != nullptr && i.doc != nullptr && l.doc != nullptr
               && xmlChoice(e, "/a/@b", {"&secret;"}) == 0 && xmlChoice(e, "/a/c", {"&secret;"}) == 0
               && xmlChoice(e, "/a/d", {"&remote;"}) == 0 && xmlChoice(p, "/a", {"&p;"}) == 0
               && xmlChoice(i, "/a", {"&name;"}) == 0 && xmlChoice(i, "/a/@n", {"&name;"}) == 0
               && xmlChoice(l, "/lolz", {"&lol9;"}) == 0 && xmlChoice(l, "/lolz/@a", {"&lol9;"}) == 0;
    };
    CHECK(inert());
    if (HeapCalls::counted())
    {
        // Parsing the laughs allocates the document and its copy of the text, a few blocks, not a billion lols.
        long calls = 0;
        {
            const HeapCalls counting;
            const Document l(laughs);
            calls = counting.count();
            CHECK(l.doc != nullptr);
        }
        CHECK(calls <= 8);
    }
#if defined(__linux__)
    // And fetch nothing: in a child that is killed if it opens a file or a socket, each is parsed and read the same.
    // (That the filter kills: a child that opens the file is killed.)
    CHECK(confined([&] { return std::fopen(secret.string().c_str(), "rb") != nullptr; }) == -SIGSYS);
    const int status = confined(inert);
    if (status == 2)
        WARN("seccomp's filters are not available here: the system calls are not checked");
    else
        CHECK(status == 0);
#endif
    std::filesystem::remove(secret);
}

TEST_CASE("what a document holds besides elements: CDATA, comments, processing instructions, references, prefixes",
          "[runtime][xml]")
{
    const Document d(R"(<?xml version="1.0"?>
<!DOCTYPE r [<!ATTLIST r default CDATA "9">]>
<r xmlns="urn:default" xmlns:s="urn:s" s:gain="0.5" gain="0.25">
    <cdata><![CDATA[2.5]]></cdata>
    <mixed>1<![CDATA[2]]>3</mixed>
    <split>1<!-- a comment -->2<?pi data?>3</split>
    <refs a="&#49;&#x2E;5" b="&lt;&amp;&gt;&quot;&apos;">&#x32;&#46;&#53;</refs>
    <spaces a="  7&#9;"> &#10; 8 &#13; </spaces>
    <s:wave>saw</s:wave>
    <empty/><blank>   </blank><child><x>1</x></child>
    <unicode name="&#xe9;t&#xe9;">&#x1F600;</unicode>
</r>)");
    REQUIRE(d.doc != nullptr);
    CHECK(xmlFloat(d, "/r/cdata", 0.0f) == 2.5f);   // a CDATA section is text
    CHECK(xmlInt(d, "/r/mixed", 0) == 1);           // the first text child only
    CHECK(xmlInt(d, "/r/split", 0) == 123);         // one text either side of a comment or a PI
    CHECK(xmlFloat(d, "/r/refs/@a", 0.0f) == 1.5f);
    CHECK(xmlFloat(d, "/r/refs", 0.0f) == 2.5f);
    CHECK(xmlChoice(d, "/r/refs/@b", {"<&>\"'"}) == 0);
    CHECK(xmlInt(d, "/r/spaces/@a", 0) == 7);       // XML's white space around a value, references among it
    CHECK(xmlInt(d, "/r/spaces", 0) == 8);
    // A name is matched as written, its prefix too: no namespaces, so the default one changes nothing.
    CHECK(xmlFloat(d, "/r/@s:gain", 0.0f) == 0.5f);
    CHECK(xmlFloat(d, "/r/@gain", 0.0f) == 0.25f);
    CHECK(xmlChoice(d, "/r/s:wave", {"saw"}) == 0);
    CHECK(xmlChoice(d, "/r/wave", {"saw"}) == -1);
    CHECK(xmlChoice(d, "/r/@xmlns", {"urn:default"}) == 0);
    CHECK(xmlInt(d, "/r/@default", -1) == -1);      // the DOCTYPE's default attribute: the DOCTYPE is skipped
    // An element with no text, or white space only, has the empty text; one with children only, too.
    CHECK(xmlChoice(d, "/r/empty", {"x", ""}) == 1);
    CHECK(xmlChoice(d, "/r/blank", {""}) == 0);
    CHECK(xmlChoice(d, "/r/child", {""}) == 0);
    CHECK(xmlFloat(d, "/r/empty", 9.0f) == 9.0f);
    CHECK(xmlChoice(d, "/r/unicode/@name", {"\xc3\xa9t\xc3\xa9"}) == 0);
    CHECK(xmlChoice(d, "/r/unicode", {"\xf0\x9f\x98\x80"}) == 0);
}

TEST_CASE("a character reference to U+0000 in a value is no XML: refused, never a value cut short", "[runtime][xml]")
{
    // XML forbids it (Legal Character); pugixml would make it a NUL, and the value would end there.
    for (const std::string_view text :
         {"<v>1.5&#0;junk</v>", "<v a='2&#0;x'/>", "<m>cubic&#x0;evil</m>", "<v>&#00000;</v>", "<v>1&#x00;</v>",
          "<r><v>ok</v><w>2&#4294967296;</w></r>",   // pugixml keeps a reference's value in 32 bits: 2^32 is 0
          "<r><!-- &#0; -->&#0;</r>", "<r a='&#0;'><![CDATA[&#0;]]></r>"})
    {
        INFO(text);
        CHECK_FALSE(parses(text));
    }
    // Where it is text, not a reference, it is allowed: in a comment, a CDATA section, a processing instruction. And
    // what is no reference to U+0000 is expanded or kept as before.
    const Document d("<r><!-- &#0; --><?pi &#0;?><c><![CDATA[&#0;]]></c><v>1&#1;</v><w a='&#;&#x;&#X0;'>&#48;</w></r>");
    REQUIRE(d.doc != nullptr);
    CHECK(xmlChoice(d, "/r/c", {"&#0;"}) == 0);
    CHECK(xmlChoice(d, "/r/v", {"1\x01"}) == 0);
    CHECK(xmlChoice(d, "/r/w/@a", {"&#;&#x;&#X0;"}) == 0);
    CHECK(xmlInt(d, "/r/w", -1) == 0);
}

TEST_CASE("a 10 MB document of number lists parses, and xml_floats reads every one of them", "[runtime][xml]")
{
    std::string text = "<pack>";
    std::int64_t expected = 0;
    int n = 0;
    int tables = 0;
    while (text.size() < 10 * 1000 * 1000)
    {
        text += tables % 2 == 0 ? "<table>" : "<table values=\"";
        for (int k = 0; k < 10000; ++k)
        {
            const int value = static_cast<int>(std::int64_t{n} * 7919 % 20001) - 10000;   // -10000 to 10000
            text += (k == 0 ? "" : k % 3 == 0 ? ", " : "\n") + std::to_string(value) + ".25";   // exactly a float
            expected += 4 * value + (value < 0 ? -1 : 1);
            ++n;
        }
        text += tables % 2 == 0 ? "</table>" : "\"/>";
        ++tables;
    }
    text += "</pack>";
    const Document d(text);
    REQUIRE(d.doc != nullptr);
    CHECK(xmlCount(d, "/pack/table") == tables);
    std::vector<float> out(10000);
    std::int64_t sum = 0;
    std::int64_t count = 0;
    for (std::int32_t t = 0; t < tables; ++t)
    {
        const std::string path = t % 2 == 0 ? "/pack/table[%d]" : "/pack/table[%d]/@values";
        const std::int32_t m = fminus_rt_xml_floats(d.doc, prepared(path).data(), &t, 1, out.data(), 10000);
        REQUIRE(m == 10000);
        count += m;
        for (float x : out)
            sum += static_cast<std::int64_t>(4 * x);
    }
    CHECK(count == n);
    CHECK(sum == expected);
}

TEST_CASE("xml_float is the nearest float to the number's digits, never a double narrowed", "[runtime][xml]")
{
    const Document d(R"(<n zero="0" minus-zero="-0" tenth="0.1" plus="+1.5" lead="007.5" point=".5" bare="1."
                         inf="INF" minus-inf="-INF" nan="NaN" huge="1e39" tiny="1e-46" least="1.4e-45"
                         max="3.4028235e38" big="1e300" integer="123456789" spaced=" 0.5 " comma="0,5" two="1 2"
                         hex="0x10" word="half" empty="" text="true" sign="-" e="1e">
                         <r>1.00000005960464477550</r><r>16777217.000000001</r><r>1.000000059604644775390625</r>
                         <exp>1E+2</exp><exp>1e-2</exp><exp>-1E-0</exp></n>)");
    REQUIRE(d.doc != nullptr);
    CHECK(sameBits(xmlFloat(d, "/n/@zero", 9.0f), 0.0f));
    CHECK(sameBits(xmlFloat(d, "/n/@minus-zero", 9.0f), -0.0f));
    CHECK(xmlFloat(d, "/n/@tenth", 9.0f) == 0.1f);
    CHECK(xmlFloat(d, "/n/@plus", 9.0f) == 1.5f);
    CHECK(xmlFloat(d, "/n/@lead", 9.0f) == 7.5f);
    CHECK(xmlFloat(d, "/n/@point", 9.0f) == 0.5f);
    CHECK(xmlFloat(d, "/n/@bare", 9.0f) == 1.0f);
    CHECK(xmlFloat(d, "/n/@inf", 9.0f) == std::numeric_limits<float>::infinity());
    CHECK(xmlFloat(d, "/n/@minus-inf", 9.0f) == -std::numeric_limits<float>::infinity());
    CHECK(std::isnan(xmlFloat(d, "/n/@nan", 9.0f)));
    CHECK(xmlFloat(d, "/n/exp[0]", 9.0f) == 100.0f);
    CHECK(xmlFloat(d, "/n/exp[1]", 9.0f) == 0.01f);
    CHECK(xmlFloat(d, "/n/exp[2]", 9.0f) == -1.0f);
    // Past halfway between two floats by less than a double can see (JsonTest.cpp has the narrowed double's answer).
    CHECK(xmlFloat(d, "/n/r[0]", 9.0f) == std::nextafter(1.0f, 2.0f));
    CHECK(xmlFloat(d, "/n/r[1]", 9.0f) == 16777218.0f);
    CHECK(xmlFloat(d, "/n/r[2]", 9.0f) == 1.0f);
    CHECK(xmlFloat(d, "/n/@huge", 9.0f) == std::numeric_limits<float>::infinity());
    CHECK(xmlFloat(d, "/n/@big", 9.0f) == std::numeric_limits<float>::infinity());
    CHECK(sameBits(xmlFloat(d, "/n/@tiny", 9.0f), 0.0f));
    CHECK(xmlFloat(d, "/n/@least", 9.0f) == std::numeric_limits<float>::denorm_min());
    CHECK(xmlFloat(d, "/n/@max", 9.0f) == std::numeric_limits<float>::max());
    CHECK(xmlFloat(d, "/n/@integer", 9.0f) == 123456792.0f);
    CHECK(xmlFloat(d, "/n/@spaced", 9.0f) == 0.5f);
    // Not one number: the fallback, whatever the value is.
    for (std::string_view other : {"/n/@comma", "/n/@two", "/n/@hex", "/n/@word", "/n/@empty", "/n/@text", "/n/@sign",
                                   "/n/@e", "/n/@missing", "/n", "/n/missing", "/n/r[3]"})
    {
        INFO(other);
        CHECK(xmlFloat(d, other, 9.0f) == 9.0f);
    }
    // xml_double: the nearest double, which is not the float widened.
    CHECK(xmlDouble(d, "/n/@tenth", 9.0) == 0.1);
    CHECK(xmlDouble(d, "/n/@big", 9.0) == 1e300);
    CHECK(xmlDouble(d, "/n/r[1]", 9.0) == 16777217.0);
    CHECK(xmlDouble(d, "/n/@plus", 9.0) == 1.5);
    CHECK(xmlDouble(d, "/n/@comma", 9.0) == 9.0);
}

TEST_CASE("xml_float reads the same digits the same way in a locale that writes a decimal comma", "[runtime][xml]")
{
    // As JsonTest.cpp's: a German or French locale, where C's strtod (pugixml's as_float) reads "0.5" as 0.
    const std::string before = std::setlocale(LC_ALL, nullptr);
    const char* found = nullptr;
    for (const char* name : {"de_DE.UTF-8", "de_DE.utf8", "de_DE", "fr_FR.UTF-8", "fr_FR.utf8", "German_Germany.1252"})
    {
        if ((found = std::setlocale(LC_ALL, name)) != nullptr)
            break;
    }
    if (found == nullptr)
        SKIP("no locale with a decimal comma on this machine");
    const double cLibrary = std::strtod("0.5", nullptr);
    const Document d(R"(<p half="0.5" comma="0,5"><pi>3.14159265</pi><list>1.25, -2.5e-1</list></p>)");
    float list[2] = {};
    const float half = xmlFloat(d, "/p/@half", 9.0f);
    const float comma = xmlFloat(d, "/p/@comma", 9.0f);
    const double pi = xmlDouble(d, "/p/pi", 9.0);
    const std::int32_t n = xmlFloats(d, "/p/list", list, 2);
    std::setlocale(LC_ALL, before.c_str());
    INFO(found);
    CHECK(cLibrary == 0.0);   // the locale is in effect: the C library stops at the '.'
    CHECK(half == 0.5f);
    CHECK(comma == 9.0f);     // and a comma is no decimal point, whatever the locale
    CHECK(pi == 3.14159265);
    CHECK(n == 2);
    CHECK(list[0] == 1.25f);
    CHECK(list[1] == -0.25f);
}

TEST_CASE("xml_int is the value when it is an integer an int holds, and never truncates or saturates", "[runtime][xml]")
{
    const Document d(R"(<i><v>7</v><v>+7</v><v>-0</v><v>007</v><v> 7.0 </v><v>700e-2</v><v>1e9</v><v>2147483647</v>
                         <v>-2147483648</v><v>7.</v>
                         <v>1.5</v><v>3e9</v><v>2147483648</v><v>-2147483649</v><v>INF</v><v>NaN</v><v>0x10</v>
                         <v>1 2</v><v>true</v><v></v><v>seven</v><v>1,0</v></i>)");
    REQUIRE(d.doc != nullptr);
    const std::int32_t integers[] = {7, 7, 0, 7, 7, 7, 1000000000, 2147483647, -2147483647 - 1, 7};
    for (std::int32_t k = 0; k < 10; ++k)
    {
        INFO(k);
        CHECK(xmlInt(d, "/i/v[%d]", -99, {k}) == integers[k]);
    }
    CHECK(xmlCount(d, "/i/v") == 22);
    for (std::int32_t k = 10; k < 23; ++k)
    {
        INFO(k);
        CHECK(xmlInt(d, "/i/v[%d]", -99, {k}) == -99);
    }
}

TEST_CASE("xml_bool, xml_count, xml_choice: each kind of node, and the fallbacks", "[runtime][xml]")
{
    const Document d(R"(<p t="true" f="false" one="1" zero="0" upper="TRUE" yes="yes" two="2" spaced=" true ">
                         <b>true</b><b>0</b><b><![CDATA[false]]></b><b/>
                         <osc wave="saw"/><osc wave="square"/><lfo/><osc wave=" sine "/>
                         <group><osc/></group><w>saw</w><w>sa</w><w>Saw</w><e></e></p>)");
    REQUIRE(d.doc != nullptr);
    CHECK(xmlBool(d, "/p/@t", 7) == 1);
    CHECK(xmlBool(d, "/p/@f", 7) == 0);
    CHECK(xmlBool(d, "/p/@one", 7) == 1);
    CHECK(xmlBool(d, "/p/@zero", 7) == 0);
    CHECK(xmlBool(d, "/p/@spaced", 7) == 1);
    CHECK(xmlBool(d, "/p/b", 7) == 1);
    CHECK(xmlBool(d, "/p/b[1]", 7) == 0);
    CHECK(xmlBool(d, "/p/b[2]", 7) == 0);
    for (std::string_view other : {"/p/@upper", "/p/@yes", "/p/@two", "/p/b[3]", "/p/b[4]", "/p", "/p/@missing"})
    {
        INFO(other);
        CHECK(xmlBool(d, other, 7) == 7);
    }
    // How many elements the last step matches: its parent's children of the name, others between them or not; with an
    // index, 1 or 0; an attribute, 1 or 0; the root, 1.
    CHECK(xmlCount(d, "/p/osc") == 3);
    CHECK(xmlCount(d, "/p/b") == 4);
    CHECK(xmlCount(d, "/p/osc[2]") == 1);
    CHECK(xmlCount(d, "/p/osc[3]") == 0);
    CHECK(xmlCount(d, "/p/group/osc") == 1);
    CHECK(xmlCount(d, "/p/osc/@wave") == 1);
    CHECK(xmlCount(d, "/p/lfo/@wave") == 0);
    CHECK(xmlCount(d, "/p") == 1);
    CHECK(xmlCount(d, "/q") == 0);
    CHECK(xmlCount(d, "/p/missing") == 0);
    CHECK(xmlCount(d, "/p/missing/osc") == 0);
    // Choices: byte for byte, the value's white space taken off.
    CHECK(xmlChoice(d, "/p/osc/@wave", {"sine", "saw", "square"}) == 1);
    CHECK(xmlChoice(d, "/p/osc[1]/@wave", {"sine", "saw", "square"}) == 2);
    CHECK(xmlChoice(d, "/p/osc[2]/@wave", {"sine", "saw", "square"}) == 0);
    CHECK(xmlChoice(d, "/p/w", {"saw", "saw"}) == 0);
    CHECK(xmlChoice(d, "/p/w[1]", {"saw", "sa"}) == 1);
    CHECK(xmlChoice(d, "/p/w[2]", {"saw", "sa", ""}) == -1);
    CHECK(xmlChoice(d, "/p/e", {"x", ""}) == 1);
    for (std::string_view other : {"/p/lfo/@wave", "/p/missing", "/p/w[3]"})
    {
        INFO(other);
        CHECK(xmlChoice(d, other, {"saw", ""}) == -1);
    }
}

TEST_CASE("xml_floats reads a list of numbers, at most cap, and counts them all", "[runtime][xml]")
{
    const Document d(R"(<p list="1, 2.5 ,-3e1,0.1" spaced="  1&#9;2&#10;3  " single="4">
                         <t>1 2 3 4 5 6</t><t>
                             0.5,
                             0.25
                         </t><t><![CDATA[7 8]]></t>
                         <empty/><blank>  </blank><double-comma>1,,2</double-comma><lead>,1</lead><trail>1,</trail>
                         <word>1 two</word><run>1-2</run><glued>1.5e3x</glued><nan>NaN INF -INF</nan>
                         <child><x>1</x></child></p>)");
    REQUIRE(d.doc != nullptr);
    float out[8];
    std::fill(std::begin(out), std::end(out), -1.0f);
    CHECK(xmlFloats(d, "/p/@list", out, 8) == 4);
    CHECK((out[0] == 1.0f && out[1] == 2.5f && out[2] == -30.0f && out[3] == 0.1f && out[4] == -1.0f));
    CHECK(xmlFloats(d, "/p/@spaced", out, 8) == 3);
    CHECK((out[0] == 1.0f && out[1] == 2.0f && out[2] == 3.0f));
    CHECK(xmlFloats(d, "/p/@single", out, 8) == 1);
    CHECK(out[0] == 4.0f);
    CHECK(xmlFloats(d, "/p/t[1]", out, 8) == 2);
    CHECK((out[0] == 0.5f && out[1] == 0.25f));
    CHECK(xmlFloats(d, "/p/t[2]", out, 8) == 2);
    CHECK((out[0] == 7.0f && out[1] == 8.0f));
    CHECK(xmlFloats(d, "/p/nan", out, 8) == 3);
    CHECK((std::isnan(out[0]) && out[1] == std::numeric_limits<float>::infinity()));
    // A cap shorter than the list: the first cap of them, and the count of all; a cap of 0, or no out, none.
    std::fill(std::begin(out), std::end(out), -1.0f);
    CHECK(xmlFloats(d, "/p/t", out, 4) == 6);
    CHECK((out[0] == 1.0f && out[3] == 4.0f && out[4] == -1.0f));
    std::fill(std::begin(out), std::end(out), -1.0f);
    CHECK(xmlFloats(d, "/p/t", out, 0) == 6);
    CHECK(xmlFloats(d, "/p/t", out, -5) == 6);
    CHECK(xmlFloats(d, "/p/t", nullptr, 8) == 6);
    CHECK(out[0] == -1.0f);
    // Anything in it not a number, or nothing in it: 0, and nothing written.
    for (std::string_view other : {"/p/empty", "/p/blank", "/p/double-comma", "/p/lead", "/p/trail", "/p/word",
                                   "/p/run", "/p/glued", "/p/child", "/p", "/p/missing", "/p/@missing"})
    {
        INFO(other);
        CHECK(xmlFloats(d, other, out, 8) == 0);
        CHECK(out[0] == -1.0f);
    }
}

TEST_CASE("a path: indices from 0, the root, attributes, %d in and out of range", "[runtime][xml]")
{
    const Document d(R"(<root a="1"><item>10</item><other/><item>11</item><item n="x">12</item>
                         <osc_2_x>8</osc_2_x><osc_-1_x>7</osc_-1_x><v2>9</v2><item><deep><er>13</er></deep></item>
                         <ns:e xmlns:ns="u">14</ns:e><e.f-g_h>15</e.f-g_h><caf&#xe9;>16</caf&#xe9;></root>)");
    // (an element's name is never a reference: "caf&#xe9;" is no name, and that document is not well formed)
    CHECK(d.doc == nullptr);
    const Document e(R"(<root a="1"><item>10</item><other/><item>11</item><item n="x">12</item>
                         <osc_2_x>8</osc_2_x><osc_-1_x>7</osc_-1_x><v2>9</v2><item><deep><er>13</er></deep></item>
                         <ns:e xmlns:ns="u">14</ns:e><e.f-g_h>15</e.f-g_h><caf)"
                     "\xc3\xa9"
                     R"(>16</caf)"
                     "\xc3\xa9"
                     R"(></root>)");
    REQUIRE(e.doc != nullptr);
    // The first of a name with no index; the n-th from 0 with one, other elements between them not counted.
    CHECK(xmlInt(e, "/root/item", 0) == 10);
    for (std::int32_t k = 0; k < 3; ++k)
    {
        CHECK(xmlInt(e, "/root/item[%d]", 0, {k}) == 10 + k);
        CHECK(xmlInt(e, "/root/item[" + std::to_string(k) + "]", 0) == 10 + k);
    }
    CHECK(xmlInt(e, "/root/item[3]/deep/er", 0) == 13);
    CHECK(xmlInt(e, "/root/item[3]/deep[0]/er[0]", 0) == 13);
    CHECK(xmlInt(e, "/root/item/deep/er", -1) == -1);   // the first item has no deep
    CHECK(xmlChoice(e, "/root/item[2]/@n", {"x"}) == 0);
    CHECK(xmlInt(e, "/root/@a", 0) == 1);
    CHECK(xmlInt(e, "/root[0]/@a", 0) == 1);
    CHECK(xmlInt(e, "/root[1]/@a", -1) == -1);
    CHECK(xmlInt(e, "/other/@a", -1) == -1);           // the first step names the root
    CHECK(xmlInt(e, "/item", -1) == -1);
    CHECK(xmlInt(e, "/root/ns:e", 0) == 14);
    CHECK(xmlInt(e, "/root/e.f-g_h", 0) == 15);
    CHECK(xmlInt(e, "/root/caf\xc3\xa9", 0) == 16);
    // Out of range, negative, and the ints' extremes: nothing there.
    CHECK(xmlInt(e, "/root/item[%d]", -1, {4}) == -1);
    CHECK(xmlInt(e, "/root/item[%d]", -1, {-1}) == -1);
    CHECK(xmlInt(e, "/root/item[%d]", -1, {std::numeric_limits<std::int32_t>::min()}) == -1);
    CHECK(xmlInt(e, "/root/item[%d]", -1, {std::numeric_limits<std::int32_t>::max()}) == -1);
    CHECK(xmlInt(e, "/root/item[2147483647]", -1) == -1);
    CHECK(xmlCount(e, "/root/item[%d]", {-1}) == 0);
    // %d in a name: the int in decimal, a minus before a negative one.
    CHECK(xmlInt(e, "/root/osc_%d_x", 0, {2}) == 8);
    CHECK(xmlInt(e, "/root/osc_%d_x", 0, {-1}) == 7);
    CHECK(xmlInt(e, "/root/v%d", 0, {2}) == 9);
    CHECK(xmlInt(e, "/root/v%d", -1, {3}) == -1);
    CHECK(xmlInt(e, "/root/osc_%d_x/@%d", -1, {2, 0}) == -1);
    // Two places, the second an index: the ints in order.
    CHECK(xmlInt(e, "/root/osc_%d_x[%d]", -1, {2, 0}) == 8);
    CHECK(xmlInt(e, "/root/osc_%d_x[%d]", -1, {2, 1}) == -1);
    // A place without an int (the compiler never makes one): nothing there.
    CHECK(xmlInt(e, "/root/item[%d]", -1) == -1);
    CHECK(fminus_rt_xml_int(e.doc, prepared("/root/item[%d]").data(), nullptr, 1, -1) == -1);
}

TEST_CASE("a null document is an empty one, and a malformed table reaches nothing", "[runtime][xml]")
{
    const Document none("not XML");
    REQUIRE(none.doc == nullptr);
    float out[2] = {-1, -1};
    CHECK(xmlFloat(none, "/a", 9.0f) == 9.0f);
    CHECK(xmlDouble(none, "/a", 9.0) == 9.0);
    CHECK(xmlInt(none, "/a/@b", 9) == 9);
    CHECK(xmlBool(none, "/a", 9) == 9);
    CHECK(xmlCount(none, "/a") == 0);
    CHECK(xmlFloats(none, "/a", out, 2) == 0);
    CHECK(xmlChoice(none, "/a", {"a", ""}) == -1);
    CHECK(out[0] == -1.0f);

    // The runtime trusts no word of a table: a count or a piece past its end, a piece outside its bytes, an unknown
    // kind, an attribute that is not last or names the root's, an index missing or negative, no step at all.
    const Document d(R"(<a b="1">2<c>3</c></a>)");
    REQUIRE(d.doc != nullptr);
    const std::int32_t good[] = {4, 1, 0, 1, 0, 1, 'a'};
    CHECK(fminus_rt_xml_int(d.doc, good, nullptr, 0, -1) == 2);
    const std::int32_t indexed[] = {5, 1, 1, 1, 0, 1, 0, 'a'};
    CHECK(fminus_rt_xml_int(d.doc, indexed, nullptr, 0, -1) == 2);
    const std::int32_t attribute[] = {8, 2, 0, 1, 0, 1, 3, 1, 1, 1, 'a' | 'b' << 8};
    CHECK(fminus_rt_xml_int(d.doc, attribute, nullptr, 0, -1) == 1);
    for (const std::vector<std::int32_t>& wrong : std::vector<std::vector<std::int32_t>>{
             {4, 1, 0, 2, 0, 1, 'a'},                       // a count past the words
             {4, 1, 0, 1, 0, 2, 'a'},                       // a piece past the bytes
             {4, 1, 0, 1, -1, 1, 'a'},                      // a piece before them
             {4, 1, 0, 1, 0, -3, 'a'},                      // a piece of no kind
             {4, 1, 7, 1, 0, 1, 'a'},                       // a step of no kind
             {4, 1, 3, 1, 0, 1, 'a'},                       // an attribute of the document
             {12, 2, 0, 1, 0, 1, 3, 1, 1, 1, 0, 1, 0, 1, 'a' | 'b' << 8},   // an attribute before the last step
             {4, 1, 1, 1, 0, 1, 'a'},                       // an index missing
             {5, 1, 1, 1, 0, 1, -1, 'a'},                   // an index negative
             {5, 1, 2, 1, 0, 1, 0, 'a'},                    // an index's int missing
             {1, 0, 0},                                     // no name's count
             {0, 0},                                        // no step
             {-3, 1},                                       // negative counts
         })
    {
        CHECK(fminus_rt_xml_int(d.doc, wrong.data(), nullptr, 0, -1) == -1);
        CHECK(fminus_rt_xml_count(d.doc, wrong.data(), nullptr, 0) == 0);
    }
    CHECK(fminus_rt_xml_int(d.doc, nullptr, nullptr, 0, -1) == -1);
    const std::int32_t choices[] = {1, 5, 9, 'a'};   // a length past the bytes
    CHECK(fminus_rt_xml_choice(d.doc, good, nullptr, 0, choices) == -1);
}

TEST_CASE("a program reads a document through the JIT: each builtin, its fallback, %d", "[codegen][xml]")
{
    const Reads reads;
    void* doc = reads.parse(kPreset.data(), static_cast<int>(kPreset.size()));
    REQUIRE(doc != nullptr);
    CHECK(reads.cutoff(doc) == 2500.5f);
    CHECK(reads.gain(doc) == 0.1);
    CHECK(reads.voices(doc) == 6);
    CHECK(reads.sync(doc, 0));
    CHECK(!reads.sync(doc, 1));
    CHECK(reads.sync(doc, 2));   // the fallback
    CHECK(reads.mode(doc, 0));
    CHECK(!reads.mode(doc, 1));
    CHECK(reads.oscillators(doc) == 3);
    CHECK(reads.detune(doc, 0) == 0.25f);
    CHECK(reads.detune(doc, 1) == -3.0f);
    CHECK(reads.detune(doc, 2) == 0.001f);
    CHECK(reads.detune(doc, 3) == 100.0f);
    CHECK(reads.detune(doc, -1) == 100.0f);
    CHECK(reads.wave(doc, 0) == 1);
    CHECK(reads.wave(doc, 1) == 2);
    CHECK(reads.wave(doc, 2) == -1);
    float row[4] = {-1, -1, -1, -1};
    CHECK(reads.table(doc, 1, row, 4) == 2);
    CHECK((row[0] == 4.5f && row[1] == 5.5f && row[2] == -1.0f));
    CHECK(reads.table(doc, 0, row, 2) == 3);
    CHECK((row[0] == 1.0f && row[1] == 2.0f && row[2] == -1.0f));
    CHECK(reads.table(doc, 2, row, 4) == 0);
    CHECK(reads.level(doc, 1) == 0.75f);
    CHECK(reads.level(doc, 0) == 9.0f);
    CHECK(reads.prefixed(doc) == 42);
    CHECK(reads.version(doc) == 2);
    reads.release(doc);
    // A null document: every fallback.
    CHECK(reads.cutoff(nullptr) == 1000.0f);
    CHECK(reads.voices(nullptr) == 8);
    CHECK(reads.oscillators(nullptr) == 0);
    CHECK(reads.wave(nullptr, 0) == -1);
    CHECK(reads.table(nullptr, 0, row, 4) == 0);
    CHECK(reads.parse("<a>", 3) == nullptr);
    reads.release(nullptr);
}

TEST_CASE("every read allocates nothing and makes no system call; parsing allocates", "[runtime][xml]")
{
    const Reads reads;
    void* doc = reads.parse(kPreset.data(), static_cast<int>(kPreset.size()));
    REQUIRE(doc != nullptr);
    float out[8];
    // Every read of the program's, once; what they give, summed.
    const auto readAll = [&] {
        double sum = reads.gain(doc) + reads.voices(doc) + reads.sync(doc, 0) + reads.oscillators(doc)
                     + reads.wave(doc, 1) + reads.prefixed(doc) + reads.version(doc) + reads.mode(doc, 1);
        sum += static_cast<double>(reads.cutoff(doc)) + static_cast<double>(reads.level(doc, 1));
        for (int k = -1; k < 4; ++k)
            sum += static_cast<double>(reads.detune(doc, k)) + reads.table(doc, k, out, 8) + reads.wave(doc, k);
        sum += static_cast<double>(out[0]);
        sum += static_cast<double>(reads.cutoff(nullptr)) + reads.table(nullptr, 0, out, 8);
        return sum;
    };
    const double expected = readAll();   // and every function called once before it is counted

    if (HeapCalls::counted())
    {
        long reading = 0;
        bool same = true;
        {
            const HeapCalls calls;
            for (int i = 0; i < 100; ++i)
                same = same && readAll() == expected;
            reading = calls.count();
        }
        CHECK(same);
        CHECK(reading == 0);
        long parsing = 0;
        {
            const HeapCalls calls;
            void* other = reads.parse(kPreset.data(), static_cast<int>(kPreset.size()));
            reads.release(other);
            parsing = calls.count();
        }
        CHECK(parsing >= 2);   // the counter sees the runtime's heap: a document, and its free
    }
#if defined(__linux__)
    // In a child under seccomp's strict mode, where any system call but read, write and exit kills it (no futex, no
    // mmap, no brk): the reads, and their sum written back.
    int fds[2];
    REQUIRE(::pipe(fds) == 0);
    const pid_t child = ::fork();
    REQUIRE(child >= 0);
    if (child == 0)
    {
        ::close(fds[0]);
        if (::prctl(PR_SET_SECCOMP, SECCOMP_MODE_STRICT) != 0)
            ::syscall(SYS_exit, 2);   // no seccomp here
        const double sum = readAll();
        const ssize_t written = ::write(fds[1], &sum, sizeof sum);
        ::syscall(SYS_exit, written == sizeof sum ? 0 : 1);
    }
    ::close(fds[1]);
    int status = 0;
    ::waitpid(child, &status, 0);
    double sum = 0;
    const ssize_t got = ::read(fds[0], &sum, sizeof sum);
    ::close(fds[0]);
    INFO("status " << status);
    if (WIFEXITED(status) && WEXITSTATUS(status) == 2)
        WARN("seccomp's strict mode is not available here: the system calls are not checked");
    else
    {
        CHECK(WIFEXITED(status));
        CHECK(WEXITSTATUS(status) == 0);
        CHECK(got == static_cast<ssize_t>(sizeof sum));
        CHECK(sum == expected);
    }
#endif
    reads.release(doc);
}
#endif

TEST_CASE("the effects: xml_parse and xml_free off the audio thread, every read on it", "[codegen][xml][effects]")
{
    using namespace fminus::ir::detail;
    for (std::string_view name : {"xml_parse", "xml_free"})
    {
        INFO(name);
        CHECK(allocates(name));
        CHECK(!blocks(name));
    }
    for (std::string_view name :
         {"xml_float", "xml_double", "xml_int", "xml_bool", "xml_count", "xml_floats", "xml_choice"})
    {
        INFO(name);
        CHECK(!allocates(name));
        CHECK(!blocks(name));
    }
    if (FMINUS_HAS_XML == 0)
        return;   // a call of one is an error of its own (below)
    std::string errors;
    auto jit = compile("module Preset\n"
                       "{\n"
                       "    void* doc;\n"
                       "    float cutoff;\n"
                       "    bool load(const void* data, int bytes)\n"
                       "    {\n"
                       "        doc = xml_parse(data, bytes);\n"
                       "        if (!doc)\n"
                       "            return false;\n"
                       "        return true;\n"
                       "    }\n"
                       "    void process(float* out, int n) [[clang::nonblocking]]\n"
                       "    {\n"
                       "        cutoff = xml_float(doc, \"/p/@cutoff\", cutoff);\n"
                       "        for (int i = 0; i < n; ++i)\n"
                       "            out[i] = cutoff + xml_floats(doc, \"/p/table\", out, 0)\n"
                       "                     + xml_count(doc, \"/p/t\") + xml_int(doc, \"/p/i\", 0)\n"
                       "                     + xml_bool(doc, \"/p/b\", false) + xml_choice(doc, \"/p/c\", \"a\")\n"
                       "                     + (float)xml_double(doc, \"/p/d\", 0.0);\n"
                       "    }\n"
                       "    void end() { xml_free(doc); }\n"
                       "};\n",
                       &errors);
    INFO(errors);
    CHECK(jit);
    CHECK(errorsOf("void* open(const void* data, int bytes) { return xml_parse(data, bytes); }\n"
                   "float process(const void* data, int bytes) [[clang::nonblocking]]\n"
                   "{\n"
                   "    void* doc = open(data, bytes);\n"
                   "    const float x = xml_float(doc, \"/x\", 0.0f);\n"
                   "    xml_free(doc);\n"
                   "    return x;\n"
                   "}\n"
                   "void close(void* doc) [[clang::nonallocating]] { xml_free(doc); }\n"
                   "void* again(const void* data) [[clang::nonallocating]] { return xml_parse(data, 4); }\n")
          == "test.fm:4:17: error: function with 'nonblocking' attribute must not call non-'nonblocking' function "
             "'open'\n"
             "test.fm:1:50: note: function cannot be inferred 'nonblocking' because it calls non-'nonblocking' "
             "function 'xml_parse'\n"
             "test.fm:6:5: error: function with 'nonblocking' attribute must not call non-'nonblocking' function "
             "'xml_free'\n"
             "test.fm:9:50: error: function with 'nonallocating' attribute must not call non-'nonallocating' function "
             "'xml_free'\n"
             "test.fm:10:65: error: function with 'nonallocating' attribute must not call non-'nonallocating' "
             "function 'xml_parse'\n");
}

TEST_CASE("an XML path's steps, prepared: names, indices, places, an attribute", "[codegen][xml]")
{
    using fminus::ir::detail::scanXmlPath;
    // Each step: its kind, its name's pieces, and its index (Runtime.h).
    const auto code = [](std::string_view path) { return scanXmlPath(path).code; };
    CHECK(code("/a") == std::vector<std::int32_t>{0, 1, 0, 1});
    CHECK(code("/a/bc") == std::vector<std::int32_t>{0, 1, 0, 1, 0, 1, 1, 2});
    CHECK(code("/a[3]") == std::vector<std::int32_t>{1, 1, 0, 1, 3});
    CHECK(code("/a[%d]/@b") == std::vector<std::int32_t>{2, 1, 0, 1, 0, 3, 1, 1, 1});
    CHECK(code("/a%d_x[%d]") == std::vector<std::int32_t>{2, 3, 0, 1, 0, -1, 1, 2, 1});
    CHECK(code("/a[0012]") == std::vector<std::int32_t>{1, 1, 0, 1, 12});
    CHECK(scanXmlPath("/p:a/@x:b").bytes == "p:ax:b");
    CHECK(scanXmlPath("/a%%").bytes == "a%");
    CHECK(scanXmlPath("/a[2147483647]").errors.empty());
}

#if FMINUS_HAS_XML
TEST_CASE("an XML path's errors are at its own bytes, and its ints are a format's", "[codegen][xml]")
{
    const auto error = [](std::string_view path) {
        return errorsOf("float f(const void* d, int k) { return xml_float(d, \"" + std::string(path)
                        + "\", 0.0f); }\n");
    };
    // Its first byte is at column 54, and an empty one's quote at 53.
    CHECK(error("preset") == "test.fm:1:54: error: an XML path must begin with '/' and the root element's name\n");
    CHECK(error("") == "test.fm:1:53: error: an XML path must begin with '/' and the root element's name\n");
    CHECK(error("/@a") == "test.fm:1:55: error: the first step of an XML path is the root element's name, not an "
                          "attribute\n");
    CHECK(error("/a//b") == "test.fm:1:57: error: expected an element's name after '/' in an XML path\n");
    CHECK(error("/a/") == "test.fm:1:56: error: expected an element's name after '/' in an XML path\n");
    CHECK(error("/a/@") == "test.fm:1:57: error: expected an attribute's name after '@' in an XML path\n");
    CHECK(error("/a/@[0]") == "test.fm:1:58: error: expected an attribute's name after '@' in an XML path\n");
    CHECK(error("/a/@b/c") == "test.fm:1:59: error: an attribute must be the last step of an XML path\n");
    CHECK(error("/a/@b[0]") == "test.fm:1:59: error: an attribute takes no index in an XML path\n");
    CHECK(error("/a[x]") == "test.fm:1:57: error: expected an index in '[ ]' in an XML path: digits, from 0, or %d\n");
    CHECK(error("/a[@n='1']")
          == "test.fm:1:57: error: expected an index in '[ ]' in an XML path: digits, from 0, or %d\n");
    CHECK(error("/a[-1]") == "test.fm:1:57: error: expected an index in '[ ]' in an XML path: digits, from 0, or %d\n");
    CHECK(error("/a[") == "test.fm:1:56: error: expected an index in '[ ]' in an XML path: digits, from 0, or %d\n");
    CHECK(error("/a[1") == "test.fm:1:56: error: expected ']' in an XML path\n");
    CHECK(error("/a[1 ]") == "test.fm:1:58: error: expected ']' in an XML path\n");
    CHECK(error("/a[2147483648]") == "test.fm:1:57: error: an index in an XML path must be at most 2147483647\n");
    CHECK(error("/a[0]b") == "test.fm:1:59: error: expected '/' after ']' in an XML path\n");
    CHECK(error("/a/*") == "test.fm:1:57: error: unexpected '*' in an XML path: a step is a name, an index '[n]' "
                           "after it, or a last '@' and an attribute's name\n");
    CHECK(error("/a/b c") == "test.fm:1:58: error: unexpected ' ' in an XML path: a step is a name, an index '[n]' "
                             "after it, or a last '@' and an attribute's name\n");
    CHECK(error("/a/text()") == "test.fm:1:61: error: unexpected '(' in an XML path: a step is a name, an index "
                                "'[n]' after it, or a last '@' and an attribute's name\n");
    CHECK(error("/a/\\x01") == "test.fm:1:57: error: unexpected '\\x01' in an XML path: a step is a name, an index "
                                "'[n]' after it, or a last '@' and an attribute's name\n");
    // Its places: %d and %%, checked as a format's (Strings.cpp), at clang's columns.
    CHECK(errorsOf("float a(const void* d, int k) { return xml_float(d, \"/osc_%f\", k, 0.0f); }\n")
          == "test.fm:1:60: error: invalid conversion specifier 'f'\n"
             "test.fm:1:60: note: a format here takes %d, an int, and %%, a percent sign\n");
    CHECK(errorsOf("float b(const void* d) { return xml_float(d, \"/osc[%d]/x\", 0.0f); }\n")
          == "test.fm:1:53: error: more '%' conversions than data arguments\n");
    CHECK(errorsOf("int c(const void* d, int k) { return xml_count(d, \"/osc\", k); }\n")
          == "test.fm:1:59: error: data argument not used by format string\n");
    CHECK(errorsOf("int e(const void* d, float k) { return xml_count(d, \"/osc[%d]\", k); }\n")
          == "test.fm:1:65: error: format specifies type 'int' but the argument has type 'float'\n");
    // The path and the choices are literals; xml_choice takes one at least.
    CHECK(errorsOf("int g(const void* d, int k) { return xml_count(d, k); }\n"
                   "int h(const void* d, int k) { return xml_choice(d, \"/w\", \"a\", k); }\n"
                   "int i(const void* d, int k) { return xml_choice(d, \"/w[%d]\", k); }\n"
                   "int j(const void* d) { return xml_choice(d, \"/w\"); }\n")
          == "test.fm:1:51: error: argument to 'xml_count' must be a string literal\n"
             "test.fm:2:63: error: argument to 'xml_choice' must be a string literal\n"
             "test.fm:3:38: error: too few arguments to function call, expected at least 4, have 3\n"
             "test.fm:4:31: error: too few arguments to function call, expected at least 3, have 2\n");
    // The other arguments convert as their parameters' types say.
    CHECK(errorsOf("float l(float d) { return xml_float(d, \"/a\", 0.0f); }\n"
                   "float m(const void* d, float* f) { return xml_float(d, \"/a\", f); }\n"
                   "int n(const void* d, int* o) { return xml_floats(d, \"/a\", o, 4); }\n"
                   "void o(const void* d) { xml_free(d); }\n"
                   "int p(const void* d) { return xml_floats(d, \"/a\"); }\n"
                   "void* q(const void* d) { return xml_parse(d); }\n")
          == "test.fm:1:27: error: passing 'float' to parameter of incompatible type 'const void *'\n"
             "test.fm:2:43: error: passing 'float *' to parameter of incompatible type 'float'\n"
             "test.fm:3:39: error: incompatible pointer types passing 'int *' to parameter of type 'float *'\n"
             "test.fm:4:25: error: passing 'const void *' to parameter of type 'void *' discards qualifiers\n"
             "test.fm:5:31: error: too few arguments to function call, expected at least 4, have 2\n"
             "test.fm:6:33: error: too few arguments to function call, expected 2, have 1\n");
}

TEST_CASE("a prototype of an xml_ builtin with its own types declares nothing new", "[codegen][xml]")
{
    std::string errors;
    auto jit = compile("void* xml_parse(const void* data, int bytes);\n"
                       "void xml_free(void* doc);\n"
                       "int count(const void* data, int bytes)\n"
                       "{\n"
                       "    void* doc = xml_parse(data, bytes);\n"
                       "    const int n = xml_count(doc, \"/a/b\");\n"
                       "    xml_free(doc);\n"
                       "    return n;\n"
                       "}\n",
                       &errors);
    INFO(errors);
    REQUIRE(jit);
    CHECK(lookup<int(const void*, int)>(*jit, "count")("<a><b/><b/></a>", 15) == 2);
    CHECK(errorsOf("float* xml_parse(const void* data, int bytes);\n"
                   "void xml_free(const void* doc);\n"
                   "float xml_float(const void* doc);\n"
                   "void xml_free(void* doc) {}\n")
          == "test.fm:1:8: error: conflicting types for 'xml_parse'\n"
             "test.fm:2:6: error: conflicting types for 'xml_free'\n"
             "test.fm:3:7: error: conflicting types for 'xml_float'\n"
             "test.fm:4:6: error: 'xml_free' is a builtin and cannot be redefined\n");
}
#endif

TEST_CASE("an xml_ builtin is a known name in every build, and an error that names the option in one without it",
          "[codegen][xml]")
{
    for (const auto& [name, call] : kCalls)
    {
        INFO(name);
        std::string errors;
        const auto jit = compile(call, &errors);
        if (FMINUS_HAS_XML != 0)
        {
            CHECK(jit);
            CHECK(errors.empty());
        }
        else
        {
            CHECK(!jit);
            CHECK(errors.find("error: '" + std::string(name) + "' needs the XML parser, which this build of FMinus "
                              "leaves out (FMINUS_XML=OFF)\n")
                  != std::string::npos);
        }
    }
}
