// tests/unit/XmlTextTest.cpp: text read from an XML document as a pointer and a length (runtime/Runtime.h,
// fminus_rt_xml_text), from C++, before any builtin calls it: an attribute's value and an element's text, found as the
// other reads find them and trimmed as they are, references expanded, CDATA, an element with no text, nothing at the
// path; and the promise behind it: it allocates nothing and makes no system call. In a build without FMINUS_XML
// nothing here is compiled.
#include "fminus/Config.h"

#if FMINUS_HAS_XML
#include "HeapCalls.h"
#include "StrictMode.h"
#include "ir/Lowering.h"
#include "runtime/Runtime.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    std::vector<std::int32_t> prepared(std::string_view path)
    {
        const fminus::ir::detail::PathScan scan = fminus::ir::detail::scanXmlPath(path);
        REQUIRE(scan.errors.empty());
        std::vector<std::int32_t> table{static_cast<std::int32_t>(scan.code.size()),
                                        static_cast<std::int32_t>(scan.bytes.size())};
        table.insert(table.end(), scan.code.begin(), scan.code.end());
        const std::size_t at = table.size();
        table.resize(at + (scan.bytes.size() + 3) / 4);
        std::memcpy(table.data() + at, scan.bytes.data(), scan.bytes.size());
        return table;
    }

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

    constexpr std::string_view kPreset = R"(<?xml version="1.0"?>
<preset name="  warm &amp; pad " empty="">
  <title>
    Lead &lt;1&gt; &#233;
  </title>
  <osc wave="saw"/>
  <osc wave="square">second</osc>
  <note><![CDATA[a <b> c]]></note>
  <blank></blank>
  <spaces>   </spaces>
</preset>)";
} // namespace

TEST_CASE("xml_text gives an attribute's value or an element's text, trimmed, and null where nothing is",
          "[runtime][xml]")
{
    const Document doc(kPreset);
    REQUIRE(doc.doc != nullptr);
    const auto text = [&](std::string_view path, const std::vector<std::int32_t>& ints = {})
    {
        std::int32_t length = -1;
        const char* bytes = fminus_rt_xml_text(doc.doc, prepared(path).data(), ints.data(),
                                               static_cast<std::int32_t>(ints.size()), &length);
        if (bytes == nullptr)
            return length == 0 ? std::string("(null)") : std::string("(null, a length)");
        return std::string(bytes, static_cast<std::size_t>(length));
    };
    CHECK(text("/preset/@name") == "warm & pad");
    CHECK(text("/preset/@empty").empty());
    CHECK(text("/preset/title") == "Lead <1> \xc3\xa9");
    CHECK(text("/preset/osc/@wave") == "saw");
    CHECK(text("/preset/osc[%d]/@wave", {1}) == "square");
    CHECK(text("/preset/osc[1]") == "second");
    CHECK(text("/preset/osc[0]").empty());   // an element with no text: empty, not null
    CHECK(text("/preset/note") == "a <b> c");
    CHECK(text("/preset/blank").empty());
    CHECK(text("/preset/spaces").empty());
    for (const char* path : {"/preset/none", "/preset/@none", "/preset/osc[2]", "/other", "/preset/osc[%d]"})
    {
        INFO(path);
        CHECK(text(path, {-1}) == "(null)");
    }
    std::int32_t length = -1;
    CHECK(fminus_rt_xml_text(nullptr, prepared("/preset").data(), nullptr, 0, &length) == nullptr);
    CHECK(length == 0);
    CHECK(fminus_rt_xml_text(doc.doc, prepared("/preset/osc/@wave").data(), nullptr, 0, nullptr) != nullptr);
}

TEST_CASE("xml_text allocates nothing and makes no system call", "[runtime][xml]")
{
    const Document doc(kPreset);
    REQUIRE(doc.doc != nullptr);
    const std::vector<std::int32_t> paths[] = {prepared("/preset/@name"), prepared("/preset/title"),
                                               prepared("/preset/osc[%d]/@wave"), prepared("/preset/none")};
    const auto readAll = [&]
    {
        std::int64_t sum = 0;
        const std::int32_t one = 1;
        for (const std::vector<std::int32_t>& path : paths)
        {
            std::int32_t length = 0;
            const char* bytes = fminus_rt_xml_text(doc.doc, path.data(), &one, 1, &length);
            sum += length + (bytes == nullptr ? 1000 : bytes[0]);
        }
        return sum;
    };
    const std::int64_t expected = readAll();
    CHECK(expected == 10 + 'w' + 11 + 'L' + 6 + 's' + 1000);
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
}
#endif
