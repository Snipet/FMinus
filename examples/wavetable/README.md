# Wavetable: tables from a JSON or an XML document

`wavetable.fm` is a wavetable oscillator whose tables come in a JSON or an XML document its host hands it, as a plugin
hands a preset or a wavetable pack a user loads ([docs/extending.md](../../docs/extending.md), recipes 39 and 40).
`bench.cpp` writes a pack of four band-limited cycles (sine, triangle, saw and square, 2048 samples each), as JSON and
as XML, compiles the module through `fminus::Program`, gives `Wavetable::init` the JSON's bytes and
`Wavetable::initXml` the XML's, plays the tables, compares each output with the same loop in C++ on the same floats,
and times them. Each loader is in the module only in a build with its parser (`FMINUS_JSON`, `FMINUS_XML`, both on by
default: `#ifdef __FMINUS_JSON__`), and the harness checks the ones the build has; in a build with neither, it says so
and does nothing.

```bash
build-release/bin/example-wavetable                             # time it here
build-release/bin/example-wavetable --position 0.9 --size 4096  # nearer the square, longer tables
build-dev/bin/example-wavetable --block 64                      # process called 64 samples at a time
build-dev/bin/example-wavetable --check                         # what ctest runs: the comparisons only
```

## The document

```json
{"name": "basic shapes", "size": 2048, "gain": 0.5, "interpolation": "linear",
 "tables": [{"name": "sine", "samples": [0, 0.0030679568, 0.0061358847, ...]},
            {"name": "triangle", "samples": [...]},
            {"name": "saw", "base64": "AAAAAFts1TZn..."},
            {"name": "square", "samples": [...]}]}
```

A table is an array of numbers, or the base64 of its little-endian float32s (the third here), as an editor that saves
floats as bytes writes it. `bench.cpp` writes each number in the fewest digits that read back as the same float, so the
document holds exactly the floats the C++ reference plays: FMinus reads each one correctly rounded, straight to a
float, whatever the machine's locale.

The same pack as XML: the third table in an attribute, its numbers separated by commas, the others as their element's
text, separated by white space.

```xml
<?xml version="1.0" encoding="UTF-8"?>
<!-- written by bench.cpp -->
<pack name="basic shapes" size="2048" gain="0.5" interpolation="linear">
    <table name="sine">0 0.0030679568 0.0061358847 ...</table>
    <table name="triangle">...</table>
    <table name="saw" samples="0, 0.0018276..."/>
    <table name="square">...</table>
</pack>
```

## The program

`init` runs off the audio thread, as a plugin's preset loader does. It parses the document, reads what it needs by
path, copies the tables to the heap, and frees the document:

```c
bool init(const void* data, int bytes, float sampleRate)
{
    void* doc = json_parse(data, bytes);                       // null for a text that is no JSON
    count = json_count(doc, "/tables");
    size = json_int(doc, "/size", 0);                          // 0 when absent, or not an integer an int holds
    gain = json_float(doc, "/gain", 1.0f);
    interpolation = json_choice(doc, "/interpolation", "nearest", "linear") == 0 ? Nearest : Linear;
    ...
    for (int t = 0; fine && t < count; ++t)
    {
        float* table = &tables[t * size];
        int n = json_floats(doc, "/tables/%d/samples", t, table, size);
        if (n == 0)
            n = json_base64_floats(doc, "/tables/%d/base64", t, table, size);
        fine = n == size;
    }
    json_free(doc);
    return fine;
}
```

- **A path is a JSON Pointer** (RFC 6901), and FMinus prepares it when it compiles the program: `"/tables/%d/samples"`
  is a constant table of three steps, the middle one the int after it, and the runtime never reads the text. A path
  that is no JSON Pointer is an error at its own column.
- **Nothing it reads can fail loudly.** A null document is an empty one: every read gives its fallback (`json_float`'s
  last argument), `json_count` 0, `json_floats` 0. So `init` checks once, at the end, that every table had `size`
  numbers, and refuses the document otherwise. `--check` hands it a truncated document, a table a sample short, a
  string among the samples, an empty text and a pack of no tables, and each is refused.
- **`json_floats` says how many numbers there are**, and writes no more than its cap: a table longer than `size`
  writes `size` of them and returns more, which `init` refuses too.

`initXml` is the same loader for the XML, its paths a small part of XPath:

```c
void* doc = xml_parse(data, bytes);                        // null for a text that is no XML document
count = xml_count(doc, "/pack/table");                     // how many <table>s
size = xml_int(doc, "/pack/@size", 0);                     // an attribute
...
int n = xml_floats(doc, "/pack/table[%d]", t, table, size);   // the t-th table, counted from 0
if (n == 0)
    n = xml_floats(doc, "/pack/table[%d]/@samples", t, table, size);
```

- **An index counts from 0**, as a JSON Pointer's does: `"/pack/table[0]"` is the first table, which XPath would write
  `/pack/table[1]`.
- **The same rules as JSON's**: a null document reads as an empty one, every read has its fallback, and `xml_floats`
  says how many numbers the list holds, writing no more than its cap, and 0 when anything in it is not a number.
  `--check` hands `initXml` a truncated document, a table a sample short, a word among the samples, an empty text and
  a pack of no tables, and each is refused.

`process` runs on the audio thread, and is `[[clang::nonblocking]]`: FMinus refuses it if it could reach `json_parse`,
`xml_parse`, either's free, `calloc` or `free`. It could read the document itself (every read is wait-free), but it
doesn't need to: the tables are the module's.

## Measurements

In the release preset on a Linux x86-64 container (an AVX-512 Xeon, 4 cores, clang 18), 480,000 samples at 110 Hz,
the best of 5 runs:

| | |
|---|---|
| loading the pack (84,386 bytes of JSON: `json_parse`, four reads of 2048 floats, `json_free`) | 318 us |
| loading the pack (92,228 bytes of XML: `xml_parse`, four reads of 2048 floats, `xml_free`) | 332 us |
| FMinus, `process` | 3.4 ns/sample |
| C++, the same loop | 5.5 ns/sample |

- **The outputs are bit-identical**, from either document, in one call and in blocks of 64 or 61: the same operations
  in the same order, and no fused multiply-adds on either side (`-ffp-contract=off`).
- **Loading is about 3.8 ms a megabyte**, once, off the audio thread: parsing, then reading each number's digits to a
  float as `json_floats` asks for it. A 10 MB pack would take tens of milliseconds, where nobody hears it. XML's is
  about the same, 3.6 ms a megabyte of its longer text (the XML row is from a later run than the others, the same
  way).
