// src/runtime/Xml.cpp: the XML parser, for FMinus's xml_* builtins (Runtime.h), compiled only into a build with
// FMINUS_XML. The engine is pugixml (cmake/FMinusXml.cmake), and this is the one file that knows it: a document is
// pugixml's, read once its text is within FMinus's size limit, and kept only when its elements are within the depth
// limit, which pugixml has none of. Every number is read from its text by fast_float (Numbers.h), to a float or a
// double, each correctly rounded and in no locale: pugixml's own conversions (as_float, as_int) are the C library's,
// and are never called. What the compiler gives the runtime, a prepared path and its ints (read through Tables.h, as
// Json.cpp reads JSON's), and what the builtins promise, are the runtime's own, and the JSON parser's: another engine
// would change this file and nothing else.
//
// Parsing allocates, and is for the time before the audio. Everything else reads a finished document and writes only
// what a builtin is given to write: no allocation, no lock, no system call, and a number of steps the document's size
// bounds (an element is found by looking through its parent's children, an attribute through its element's). The
// audio thread may call any of them. Nothing here recurses: the tree is walked by its parent and sibling links.
#include "runtime/Runtime.h"

#if FMINUS_HAS_XML
#include "runtime/Numbers.h"
#include "runtime/Tables.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>

#if defined(__GNUC__)
#pragma GCC visibility push(hidden)
#endif
#include <pugixml.hpp>
#if defined(__GNUC__)
#pragma GCC visibility pop
#endif

namespace
{
    using fminus::runtime::NumberSyntax;

    // How pugixml reads a document: elements, attributes, text and CDATA, the predefined and character references
    // expanded, line ends made line feeds and an attribute's white space spaces; text either side of a comment or a
    // processing instruction (neither kept) one text. No DOCTYPE kept, so no entity it declares is ever expanded.
    constexpr unsigned kParse = pugi::parse_cdata | pugi::parse_escapes | pugi::parse_eol | pugi::parse_wconv_attribute
                                | pugi::parse_merge_pcdata;

    // Whether a parsed document is one FMinus keeps: one root element, and no element deeper than the limit.
    bool fits(const pugi::xml_document& document)
    {
        int roots = 0;
        for (pugi::xml_node node = document.first_child(); node; node = node.next_sibling())
            roots += node.type() == pugi::node_element ? 1 : 0;
        if (roots != 1)
            return false;
        // Every node once, in document order, by the tree's links: down to a first child, else on to the next
        // sibling of the node or of the nearest ancestor that has one.
        const pugi::xml_node root = document.document_element();
        pugi::xml_node node = root;
        int depth = 1;
        for (;;)
        {
            if (node.type() == pugi::node_element && depth > FMINUS_XML_MAX_DEPTH)
                return false;
            if (pugi::xml_node child = node.first_child())
            {
                node = child;
                ++depth;
                continue;
            }
            while (node != root && !node.next_sibling())
            {
                node = node.parent();
                --depth;
            }
            if (node == root)
                return true;
            node = node.next_sibling();
        }
    }

    // Whether `length` bytes at `text` hold a character reference to U+0000 as pugixml reads one (strconv_escape:
    // "&#" and decimal digits, or "&#x" and hex digits, then ';', the value kept in 32 bits, so that &#4294967296;
    // is one too). XML forbids it (its Legal Character constraint); pugixml expands it to a NUL, which would end the
    // value early: "1.5&#0;junk" would read as 1.5.
    bool nulReference(const char* text, std::size_t length)
    {
        const char* const end = text + length;
        for (const char* at = text;; ++at)
        {
            at = static_cast<const char*>(std::memchr(at, '&', static_cast<std::size_t>(end - at)));
            if (at == nullptr)
                return false;
            const char* c = at + 1;
            if (c == end || *c != '#')
                continue;
            const bool hex = ++c != end && *c == 'x';
            if (hex)
                ++c;
            const char* const digits = c;
            std::uint32_t value = 0;
            for (; c != end; ++c)
            {
                const auto digit = static_cast<unsigned>(*c - '0');
                const auto letter = static_cast<unsigned>((*c | ' ') - 'a');
                if (digit <= 9)
                    value = value * (hex ? 16u : 10u) + digit;
                else if (hex && letter <= 5)
                    value = value * 16u + letter + 10u;
                else
                    break;
            }
            if (c != end && *c == ';' && c != digits && value == 0)
                return true;
        }
    }

    // Whether a document's text, parsed as it is but with no reference expanded, has a reference to U+0000 in a
    // value, an element's text or an attribute's, the only places pugixml expands one (one in a comment or a CDATA
    // section is text, and allowed); or no document at all. Only a text with such a reference somewhere is parsed this
    // way (nulReference), before the document itself is, so the memory a document takes is one tree's at most.
    bool nulInValue(const void* data, std::int32_t bytes)
    {
        pugi::xml_document raw;
        if (!raw.load_buffer(data, static_cast<std::size_t>(bytes), kParse & ~pugi::parse_escapes & ~pugi::parse_cdata,
                             pugi::encoding_utf8))
            return true;   // no document either way (an expanded reference is never an error), or no memory to tell
        for (pugi::xml_node node = raw.first_child(); node;)
        {
            if (node.type() == pugi::node_pcdata && nulReference(node.value(), std::strlen(node.value())))
                return true;
            for (const pugi::xml_attribute attribute : node.attributes())
            {
                if (nulReference(attribute.value(), std::strlen(attribute.value())))
                    return true;
            }
            // Every node once, by the tree's links, as fits() walks them.
            if (pugi::xml_node child = node.first_child())
            {
                node = child;
                continue;
            }
            while (node && !node.next_sibling())
                node = node.parent();
            if (node)
                node = node.next_sibling();
        }
        return false;
    }

    // XML's white space.
    bool space(char c)
    {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r';
    }

    // A value read: its text, the white space around it taken off.
    struct Text
    {
        const char* bytes = nullptr;
        std::int32_t length = 0;
    };

    Text trimmed(const char* text)
    {
        std::size_t length = std::strlen(text);   // within the document: at most 64 MiB
        while (length > 0 && space(*text))
        {
            ++text;
            --length;
        }
        while (length > 0 && space(text[length - 1]))
            --length;
        return {text, static_cast<std::int32_t>(length)};
    }

    // What a prepared path reaches: an element or an attribute, nothing (both empty), and with `counting`, how many
    // elements its last step matches (xml_count).
    struct Found
    {
        pugi::xml_node element;
        pugi::xml_attribute attribute;
        std::int32_t matches = 0;
    };

    // The step kinds of a prepared path (Runtime.h).
    enum Kind : std::int32_t
    {
        Element = 0,
        Indexed = 1,
        IndexedByInt = 2,
        Attribute = 3,
    };

    Found find(const void* document, const void* path, const std::int32_t* ints, std::int32_t count,
               bool counting = false)
    {
        Table table;
        if (document == nullptr || !open(path, table))
            return {};
        // The children the next step looks through: the document's (whose one element is the root) at first.
        pugi::xml_node parent = *static_cast<const pugi::xml_document*>(document);
        Found found;
        std::int32_t at = 0;
        while (at < table.words)
        {
            const std::int32_t kind = table.word(at++);
            if ((kind != Element && kind != Indexed && kind != IndexedByInt && kind != Attribute) || at == table.words)
                return {};
            const std::int32_t pieces = table.word(at++);
            if (pieces < 0 || pieces > (table.words - at) / 2)
                return {};
            const Token name{table, at, pieces, ints, count};
            at += 2 * pieces;
            std::int32_t index = 0;
            if (kind == Indexed || kind == IndexedByInt)
            {
                if (at == table.words)
                    return {};
                const std::int32_t word = table.word(at++);
                if (kind == IndexedByInt && (ints == nullptr || word < 0 || word >= count))
                    return {};
                index = kind == Indexed ? word : ints[word];
                if (index < 0)
                    return {};
            }
            const bool last = at == table.words;
            if (kind == Attribute)
            {
                if (!last || parent == *static_cast<const pugi::xml_document*>(document))
                    return {};   // an attribute ends a path, and the document has none
                for (pugi::xml_attribute a = parent.first_attribute(); a; a = a.next_attribute())
                {
                    if (name.names(a.name(), std::strlen(a.name())))
                    {
                        found.attribute = a;
                        found.matches = 1;
                        return found;
                    }
                }
                return {};
            }
            // The index-th child element of that name; on a last step that is counted with no index, all of them.
            const bool all = counting && last && kind == Element;
            pugi::xml_node match;
            std::int32_t seen = 0;
            for (pugi::xml_node child = parent.first_child(); child; child = child.next_sibling())
            {
                if (child.type() != pugi::node_element || !name.names(child.name(), std::strlen(child.name())))
                    continue;
                if (seen == index)
                    match = child;
                ++seen;
                if (match && !all)
                    break;
            }
            if (!match)
                return {};
            parent = match;
            found.matches = all ? seen : 1;
        }
        if (parent == *static_cast<const pugi::xml_document*>(document))
            return {};   // no step: the compiler makes no such path
        found.element = parent;
        return found;
    }

    // The value at a path, trimmed: an attribute's, or an element's text (its first text or CDATA child; none, the
    // empty text). False when nothing is there.
    bool valueAt(const void* document, const void* path, const std::int32_t* ints, std::int32_t count, Text& text)
    {
        const Found found = find(document, path, ints, count);
        if (found.attribute)
            text = trimmed(found.attribute.value());
        else if (found.element)
            text = trimmed(found.element.text().get());
        else
            return false;
        return true;
    }

    bool readFloat(Text text, float& value)
    {
        return text.length > 0 && fminus::runtime::readFloat(text.bytes, text.length, NumberSyntax::Xml, value)
                                      == text.length;
    }

    // The numbers of a list (xml_floats): each as readFloat reads one, separated by white space, or a comma with any
    // white space around it; each passed to number(value), until it returns false. Their count (of those passed, when
    // it stops), or -1 when anything in the list (before it stops) is not a number. An empty list (white space at
    // most) is none.
    template <typename Number>
    std::int64_t list(Text text, Number&& number)
    {
        std::int32_t at = 0;
        const auto skip = [&] {
            while (at < text.length && space(text.bytes[at]))
                ++at;
        };
        skip();
        std::int64_t numbers = 0;
        while (at < text.length)
        {
            float value = 0.0f;
            const std::int32_t read =
                fminus::runtime::readFloat(text.bytes + at, text.length - at, NumberSyntax::Xml, value);
            if (read == 0)
                return -1;
            at += read;
            ++numbers;
            if (!number(value))
                return numbers;
            const std::int32_t after = at;
            skip();
            bool comma = false;
            if (at < text.length && text.bytes[at] == ',')
            {
                comma = true;
                ++at;
                skip();
            }
            if (at == text.length)
                return comma ? -1 : numbers;
            if (at == after)
                return -1;   // a number runs into what follows it
        }
        return numbers;
    }
} // namespace

extern "C" void* fminus_rt_xml_parse(const void* data, std::int32_t bytes) noexcept
{
    if (data == nullptr || bytes <= 0 || bytes > FMINUS_XML_MAX_BYTES)
        return nullptr;
    if (nulReference(static_cast<const char*>(data), static_cast<std::size_t>(bytes))
        && nulInValue(data, bytes))
        return nullptr;   // not well formed, and a NUL in a value would cut it short
    auto* document = new (std::nothrow) pugi::xml_document;
    if (document == nullptr)
        return nullptr;
    // load_buffer copies the text (load_buffer_inplace would not): the program's bytes are never written.
    const pugi::xml_parse_result result =
        document->load_buffer(data, static_cast<std::size_t>(bytes), kParse, pugi::encoding_utf8);
    if (!result || !fits(*document))
    {
        delete document;
        return nullptr;
    }
    return document;
}

extern "C" void fminus_rt_xml_free(void* document) noexcept
{
    delete static_cast<pugi::xml_document*>(document);
}

extern "C" float fminus_rt_xml_float(const void* document, const void* path, const std::int32_t* ints,
                                     std::int32_t count, float fallback) noexcept
{
    Text text;
    float value = 0.0f;
    return valueAt(document, path, ints, count, text) && readFloat(text, value) ? value : fallback;
}

extern "C" double fminus_rt_xml_double(const void* document, const void* path, const std::int32_t* ints,
                                       std::int32_t count, double fallback) noexcept
{
    Text text;
    double value = 0.0;
    return valueAt(document, path, ints, count, text) && text.length > 0
                   && fminus::runtime::readDouble(text.bytes, text.length, NumberSyntax::Xml, value) == text.length
               ? value
               : fallback;
}

extern "C" std::int32_t fminus_rt_xml_int(const void* document, const void* path, const std::int32_t* ints,
                                          std::int32_t count, std::int32_t fallback) noexcept
{
    Text text;
    std::int32_t value = 0;
    return valueAt(document, path, ints, count, text)
                   && fminus::runtime::readInt(text.bytes, text.length, NumberSyntax::Xml, value)
               ? value
               : fallback;
}

extern "C" std::int32_t fminus_rt_xml_bool(const void* document, const void* path, const std::int32_t* ints,
                                           std::int32_t count, std::int32_t fallback) noexcept
{
    Text text;
    if (!valueAt(document, path, ints, count, text))
        return fallback;
    const auto is = [&](const char* word) {
        return static_cast<std::size_t>(text.length) == std::strlen(word)
               && std::memcmp(text.bytes, word, static_cast<std::size_t>(text.length)) == 0;
    };
    if (is("true") || is("1"))
        return 1;
    if (is("false") || is("0"))
        return 0;
    return fallback;
}

extern "C" std::int32_t fminus_rt_xml_count(const void* document, const void* path, const std::int32_t* ints,
                                            std::int32_t count) noexcept
{
    return find(document, path, ints, count, true).matches;
}

extern "C" std::int32_t fminus_rt_xml_floats(const void* document, const void* path, const std::int32_t* ints,
                                             std::int32_t count, float* out, std::int32_t cap) noexcept
{
    Text text;
    if (!valueAt(document, path, ints, count, text))
        return 0;
    // First that it is a list and how long, then the first `cap` of its numbers, in order.
    const std::int64_t numbers = list(text, [](float) { return true; });
    if (numbers <= 0)
        return 0;
    const std::int32_t n =
        out == nullptr || cap <= 0 ? 0 : static_cast<std::int32_t>(std::min<std::int64_t>(cap, numbers));
    std::int32_t written = 0;
    if (n > 0)
    {
        list(text, [&](float value) {
            out[written++] = value;
            return written < n;
        });
    }
    return static_cast<std::int32_t>(numbers);   // the document's size bounds it
}

extern "C" std::int32_t fminus_rt_xml_choice(const void* document, const void* path, const std::int32_t* ints,
                                             std::int32_t count, const void* choices) noexcept
{
    Text text;
    if (!valueAt(document, path, ints, count, text))
        return -1;
    return choose(choices, text.bytes, static_cast<std::size_t>(text.length));
}
#endif
