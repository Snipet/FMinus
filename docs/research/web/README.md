# A web library for FMinus programs, and built-in JSON and XML parsers

*Research, 2026-11-10. Nothing under `src/`, `include/` or `cmake/` changed. Two questions were asked: how to add a
web library that FMinus programs call (HTTP and the like, as they call the FFT), switchable at build time; and how to
give FMinus built-in, optimized JSON and XML parsers. They are one subject, because FMinus has no strings: a program
can do nothing with a response until something turns its bytes into numbers.*

## 0. Summary

**What I would build, in this order:**

1. **String literals as arguments of builtins** (the smallest language step that makes any of this possible; §3).
2. **JSON and XML parsers in the runtime**, on by default, each behind its own build option (§6). They need no
   network, are useful on their own (a host hands a program a file's bytes), and are the half of the work that is
   FMinus's own design.
3. **`fminus_web`, a library of its own beside `fminus_runtime`**, off by default behind `FMINUS_WEB`, as the Python
   package is behind `FMINUS_BUILD_PYTHON` (§7): an HTTP(S) client first (§5), WebSockets second, a server last or
   never.

**The four things that decide the design**, each from FMinus as it is today:

| Constraint | Consequence |
|---|---|
| No `char`, no strings, no bytes (README, "A string literal is C's, and only an attribute's argument") | URLs, keys and paths are string **literals**; payloads are opaque (`void*` and a size) until a parser or a decoder turns them into `float`s and `int`s (§3, §4) |
| The audio thread: `[[clang::nonblocking]]` is checked, and runtime functions "run inside the audio thread's call tree" (recipe 5) | Every call that can wait or allocate is refused in a nonblocking function, as `fft_create` is; what the audio thread may call (is it done, read a number, drain a ring) is wait-free (§4.4) |
| A host may compile programs its own author did not write (a playground, a preset passed between users) | The build option is not enough: a host grants the network per program, and because URLs are literals it can see every one **before the program runs** (§8) |
| Every dependency is fetched, pinned by SHA-256 and built from source; a plugin hides its symbols | A TLS stack is the expensive part of the choice, in build time, in size and in who answers its CVEs (§5) |

**Decisions that are yours** are collected in §10. The largest: whether strings stay literals-only or become C's
`const char*`; whether TLS is bundled or the platform's; and whether the parsers are embedded libraries or FMinus's
own.

## 1. What a program would do with the network

| Use | Shape | Needs |
|---|---|---|
| Load data before playing: impulse responses, wavetables, sample sets, tuning tables, presets, neural amp models | one GET, off the audio thread, then parse or decode | HTTP(S) client; JSON, XML or raw floats |
| Report: levels, a detected pitch, an analysis | a small POST now and then | HTTP(S) client; a way to write JSON from numbers |
| Live control and data: remote parameters, a sensor or data feed to sonify, a jam session's clock | a long-lived stream, messages both ways, read on the audio thread | WebSocket client; wait-free rings |
| A page that shows and controls the program | the program (or the host) serves HTTP | a server: firewall prompts, port clashes between plugin instances, a local attack surface |
| Audio over the network (radio, calls) | codecs, jitter buffers, clock recovery | not a web library's job: out of scope |

The first row is nearly all of the value and the least risk, and it is a "load, then run" shape: deterministic given
the same payload, with no network activity while audio plays. I would treat it as the product and the rest as
extensions.

What the data looks like in practice (the survey's findings, appendix B): MusicXML and Audio Unit presets are XML
that opens with a DOCTYPE, so a parser must skip one correctly and never fetch it; Ableton's sets are gzip-compressed
XML, so they need an inflate step this report does not cover; Vital's presets and wavetables are JSON, reportedly
with the tables as base64 text inside it, which a numbers-only reader cannot see; VST3 presets and SOFA files are
binary, and SFZ is plain text, so none of the three is a parser's business here. Raw little-endian `float32` is the
format a program's own server would choose, and it needs no parser at all: for large arrays it beats any text format
by orders of magnitude, so the library should decode it too, and base64 with it (§4.2).

## 2. What FMinus already has that this builds on

- **Runtime functions as builtins** (extending.md, recipes 5 and 13): a table in `Calls.cpp` from the FMinus name to
  the runtime's symbol, a line in `RuntimeSymbols.def`, attributes that say what the function touches
  (`kRuntimeFacts`, recipe 19), and the effects' list of what allocates. The FFT is the model: `fft_create` returns an
  opaque `void*`, is refused in a nonallocating function, and `fft_forward` is allowed in a nonblocking one.
- **A C library inside the runtime** (recipe 13, `cmake/FMinusFft.cmake`): fetched once into the deps cache, pinned in
  `FMinusPins.cmake`, its own CMake never added, its objects compiled with hidden visibility into the library.
- **An optional feature** (`FMINUS_BUILD_PYTHON`, the `python` preset): an option that is off by default, a pin
  fetched only when it is on, a ctest test of its own.
- **A string token.** The scanner already reads C's string literals with their escapes (`STRING`, `stringValue`,
  recipe 35); the parser takes them only as an attribute's arguments.
- **Per-program state the host sets.** The loop budget and the bounds record are globals of the program's module with
  external linkage, which the host reads and writes (`LoopBudget.cpp`, `BoundsCheck.cpp`). A program's network
  permissions and its open requests can hang off one more.
- **A generated `fminus/Config.h`**, where a host can read what this build has.
- **External prototypes resolve in the process** (architecture.md): `float atanf(float x);` calls libm's. So a host
  can already give a program functions of its own, as long as their parameters are numbers and pointers.

## 3. The language question: strings

A web library needs at least a URL, and a parser needs at least a key. Four ways to say them, from no language change
to C's whole answer:

| | What a program writes | Compiler change | What it cannot do |
|---|---|---|---|
| **A. Attributes only** | `float ir[65536] [[web::get("https://example.com/ir.f32")]];` | none: FMinus passes a field's attributes to the host as written, and the host (with a helper library) fetches, decodes and fills the field before it calls `init` | anything programmable: a choice of resource at run time, a stream, a POST, a size not known when the program is compiled |
| **B. Literals as builtin arguments** | `void* r = http_get("https://example.com/ir.json");` `float c = json_float(doc, "/filter/cutoff", 1000.0f);` | a string literal in a call's argument list, accepted only by a builtin's string parameter; lowered to a private constant and its length | hold a string in a variable, build one, or pass one to a function of the program's |
| **C. `const char*`, opaque** | the same, and `const char* base = "https://example.com";`, a `const char*` parameter, a C prototype with one (`int puts(const char* s);`) | a new pointee, `char`, that can be pointed to but never read, written or indexed (as `void` is); literals are expressions of that type | compute with characters: no `char` values, no arrays of them |
| **D. C's strings** | all of C: `char buf[64]`, `s[i]`, `'a'`, `<string.h>` as builtins | a new arithmetic type with its conversions, arrays of it, character constants, and a decision on aliasing (C lets a `char*` alias anything, which recipe 17's TBAA tree would have to carry) | nothing |

**I recommend B now, with C as where it grows.** The reasons:

- **Every string is known when the program is compiled.** That is what makes the two properties in §0 possible: the
  host can list a program's URLs before running it (§8), and the compiler can prepare a path instead of the runtime
  parsing `"/filter/cutoff"` on every call (§6.4).
- **It is a subset of C either way.** `http_get("…")` is a valid C call; B only restricts where a literal may appear.
  When C's `const char*` arrives, the same programs keep their meaning, so nothing built on B is thrown away.
- **D is a language feature in its own right** and should be decided on its own merits. With parsers built in, the
  use cases in §1 do not need it.
- **A is worth having as well, later,** as a host-side convenience over the same runtime: a module that only wants
  an impulse response in a field should not have to write a loader. It costs the compiler nothing.

**Numbers inside strings.** With literals only, a program still needs `"/osc/2/detune"` in a loop, or preset 17's
URL. The C way that needs no string type is a format with `int` arguments, checked when the program is compiled, as
clang checks `printf`'s:

```c
for (int k = 0; k < 4; ++k)
    detune[k] = json_float(doc, "/osc/%d/detune", k, 0.0f);       // %d only; the count is checked
void* r = http_get("https://example.com/presets/%d.json", index);
```

The host still sees the pattern, which is what a permission needs (`https://example.com/presets/*.json`).

## 4. The API a program would see

Names follow the FFT's (`fft_create`, `fft_forward`): lower case, a prefix per family, opaque `void*` handles, a
null handle that does nothing. This is a sketch to make the constraints concrete, not a specification.

### 4.1 HTTP

```c
module Loader
{
    void* request;
    float ir[65536];
    int   length;

    void load()                                            // the host calls it off the audio thread
    {
        request = http_get("https://example.com/rooms/hall.json");
        if (http_wait(request, 10000) == 200)              // blocks this thread: refused in a nonblocking function
        {
            void* doc = json_parse(http_body(request), http_size(request));
            length = json_floats(doc, "/ir", ir, 65536);   // as many as fit; the result is how many there are
            json_free(doc);
        }
        http_close(request);
    }
};
```

| Builtin | Does | Effect class (§4.4) |
|---|---|---|
| `http_get(url, ...)`, `http_post(url, data, bytes, content_type, ...)` | starts a request and returns its handle at once | allocates |
| `http_wait(r, timeout_ms)` | blocks until done or the time is up; the status | blocks |
| `http_status(r)` | 0 while pending, the HTTP status when done, a negative code for a transport failure, a refusal by the host's policy among them | wait-free |
| `http_size(r)`, `http_body(r)`, `http_progress(r)` | the body's bytes and its address (a `const void*`), valid from done until `http_close` | wait-free |
| `http_cancel(r)`, `http_close(r)` | stop it; free it | allocates |

Sizes are `int`s, as `malloc`'s are, so a body is at most 2 GB; the host's limit will be far lower (§8).

### 4.2 Bytes to numbers, without a parser

```c
int n = bytes_floats(http_body(r), http_size(r), ir, 65536);     // little-endian float32, as a server of floats sends
```

and its siblings for 16- and 24-bit PCM, and `json_base64(doc, path)`, which decodes a base64 string into such a
buffer (it allocates). A few lines of runtime each, and the right answer for any payload the program's author
controls. Decoding
WAV, AIFF or FLAC is a natural next request and a separate decision (each is another dependency or another decoder to
own).

### 4.3 JSON and XML

```c
void* doc = json_parse(data, bytes);                       // allocates; null for a document that is not JSON
float cutoff = json_float(doc, "/filter/cutoff", 1000.0f); // a JSON Pointer (RFC 6901); the fallback when absent
int   voices = json_int(doc, "/master/voices", 8);
bool  sync   = json_bool(doc, "/lfo/0/sync", false);
int   count  = json_count(doc, "/osc");                    // an array's elements, an object's members, else 0
int   wave   = json_choice(doc, "/osc/%d/wave", k, "sine", "saw", "square");   // which literal the string equals, or -1
int   n      = json_floats(doc, "/tables/%d", t, row, 2048);                   // the hot path: an array of numbers
json_free(doc);

void* xml = xml_parse(data, bytes);
float c  = xml_float(xml, "/preset/filter/@cutoff", 1000.0f);    // an attribute
int   m  = xml_floats(xml, "/pack/table[%d]", t, row, 2048);     // an element's text as a list of numbers
int   k  = xml_count(xml, "/preset/osc");
xml_free(xml);
```

- **`json_choice` is how a string value becomes something FMinus can hold:** an index into the literals, which is an
  `enum`'s value. There is no way to get the string itself, by design.
- **`json_floats` on an array of arrays** fills row after row, which is what a 2-D array's row or a heap block wants.
- **The XML path is a small subset of XPath:** child steps by name, `[n]`, a final `@attribute`; no axes, predicates
  or functions. Whether `[n]` counts from 1 (XPath's) or 0 (C's, and JSON Pointer's) is a choice to make once.
- **Writing JSON** (for a POST) needs the reverse: a builder with literal keys, `json_begin()`, `json_set_float(b,
  "/level", x)`, `json_end(b)`. Smaller than parsing and can wait for the POST use case.

### 4.4 Effects: three classes, one of them new

| Class | Members | `nonallocating` | `nonblocking` |
|---|---|---|---|
| allocates | `http_get`, `http_post`, `http_close`, `http_cancel`, `json_parse`, `json_base64`, `json_free`, `xml_parse`, `xml_free`, `ws_open`, `ws_close` | refused | refused |
| **blocks (new)** | `http_wait` | allowed | refused |
| wait-free | `http_status`, `http_size`, `http_body`, `http_progress`, `bytes_floats`, every `json_` and `xml_` read of a parsed document, `ws_receive_floats`, `ws_send_floats` | allowed | allowed |

`Effects.cpp` has the first and third today (`allocates(name)`, and builtins otherwise trusted). The second is one
more predicate beside it. The third is a promise the runtime must keep: a read of a parsed document touches only
memory that parsing finished writing, and a request's status and body are published with one atomic store when it
completes. That lets a nonblocking `process` poll for a late-arriving table and swap to it without the host's help.
Wait-free is not the same as cheap: `json_floats` of a million numbers on the audio thread would wait on nothing and
still take milliseconds. The manual should say to read large arrays where they are parsed, and §6.4's second level
is what would make every read cheap.

### 4.5 WebSockets (second phase)

The audio thread reads and writes through fixed-size single-producer, single-consumer rings; the library's I/O thread
is the other end. `ws_receive_floats(ws, out, cap)` takes the next binary message of floats or returns 0;
`ws_send_floats(ws, data, n)` returns 0 when the ring is full, and drops nothing silently. Text messages go through
`json_parse` off the audio thread. This is the part with real design work left (message framing for numbers, ring
sizes, what "connected" means to a `process` that must not wait), which is why it is second.

## 5. The HTTP client: which library, and whose TLS

A survey of the candidates, with what was read at the source and what was not, is in
[appendix A](#appendix-a-the-http-and-tls-survey). What it comes to:

**FMinus's license narrows the field first.** FMinus is GPLv3, so a dependency must be compatible with it. Mongoose
is "GPL-2.0-only or commercial": out. MIT, BSD, the curl license, BSL-1.0 and Apache-2.0 (mbedTLS, OpenSSL 3 and
later) are fine.

**The build rule narrows it next.** OpenSSL has no CMake build and needs Perl (and NASM and nmake on Windows), which
does not fit "fetched, pinned and built by CMake". Boost.Beast is a 103 MB archive of building blocks that still
needs OpenSSL and leaves redirects, proxies and trust stores to write. That leaves two serious clients, and the
platforms' own stacks:

| | libcurl 8.23.0 (2026-10-14) | cpp-httplib 0.65.0 (2026-11-05) | The platforms' own (WinHTTP, NSURLSession) |
|---|---|---|---|
| License | curl (MIT-like) | MIT | none to carry |
| Shape | C, CMake; a multi interface that drives many requests from one thread | one C++11 header of 24,000 lines; blocking calls, a thread a request | a C API on Windows, Objective-C++ on macOS; **nothing on Linux** |
| Covers | HTTP/1.1 (HTTP/2 with nghttp2), redirects, proxies, timeouts, streaming; a basic WebSocket client; no server | HTTP/1.1 client **and server**, redirects, proxies, timeouts; WebSocket client and server | HTTP/1.1 and 2, WebSocket clients; no server |
| TLS | Schannel on Windows (no dependency, the system's trust store); mbedTLS, the OpenSSL family, wolfSSL or GnuTLS elsewhere | OpenSSL, mbedTLS or wolfSSL | the system's, patched by the system |
| Root certificates | Windows: the store. macOS with mbedTLS: FMinus must supply them (curl's Keychain support needs OpenSSL or GnuTLS). Linux: a bundle path FMinus finds at run time | handled on all three, with any backend (the Keychain, the Windows stores, a list of distributions' bundle paths) | the system's |
| Cancel from another thread | `curl_multi_wakeup`, or a callback's return value | `Client::stop()` | closing the handle |
| Release archives | uploaded and signed, with a digest | GitHub's generated tarballs only (as FMinus pins Catch2 and PFFFT today) | n/a |
| Security record | 239 CVEs over its life and 63 in 2026, most of them low and many in protocols FMinus would compile out; fuzzed continuously | 23 advisories; in 2026, high-severity ones in exactly the code a client leans on: a credential leak across a redirect, and a certificate-chain bypass on the mbedTLS and wolfSSL path | the vendor's |
| Churn | steady | pre-1.0, a release a week | none |

**I recommend libcurl, with Schannel on Windows and mbedTLS (an LTS branch) on macOS and Linux**, built with
everything but HTTP and HTTPS turned off:

- It is the most examined HTTP client there is, and the first version of this library is nothing but an
  internet-facing HTTP client.
- Its multi interface is the shape §8 wants: one I/O thread for the process, any number of requests, a wake-up from
  another thread to cancel.
- Windows carries no TLS library at all.
- Its costs are known and bounded: FMinus loads the Keychain's roots itself on macOS (a few dozen lines against
  Security.framework, which is what cpp-httplib does), probes the usual bundle paths on Linux and lets the host name
  a file; and the pins move often. A `Scripts/pin-web.sh` beside `pin-llvm.sh` and a habit of bumping on curl's and
  mbedTLS's security releases is part of adopting it.

Two build notes from the survey: curl's CMake has about fifty switches to turn off (the executable, tests, docs,
libpsl, libssh2, libidn2, nghttp2 and each `CURL_DISABLE_*` protocol), and its feature checks misfire when the TLS
library is a sub-project of the same build. FMinus already has the answer to the second: build mbedTLS and then curl
at configure time into the deps cache's `install/`, as it builds Bison and Flex, and find them there.

**The alternative I would take seriously is the platforms' own stacks**, if "who answers the next TLS CVE" matters
more to you than one code path: on Windows and macOS the system does, the binary grows by nothing, and proxies and
trust need no code. The price is three implementations (Linux still needs curl and a TLS library), one of them in
Objective-C++.

**cpp-httplib is the fallback for a server.** If a program or a host ever serves a page, a loopback-only
cpp-httplib server without TLS, beside curl for everything outbound, keeps internet-facing TLS on the examined path.
I would not start there.

*Not measured: added binary size and build time. No upstream states either, and nothing was built here.*

## 6. The parsers: embed, or build

**What "optimized" is worth here.** The published measurements (appendix B) say three things:

- **Arrays of floats are bounded by reading the numbers, not by the structure.** On a float-heavy document (Keiser
  and Lemire, a Xeon Gold 6338): simdjson's lazy API 0.87 to 0.91 GiB/s, its DOM 0.48 to 0.51, yyjson 0.45 to 0.47,
  RapidJSON 0.24, nlohmann/json 0.044. fast_float reads random doubles at 1042 MB/s where `strtod` reads 191.
- **Knowing what you want is worth about 2x, not 10x.** The same library reading lazily instead of building a tree
  is 1.7x faster on average and about 2.6x on a single-field lookup. Larger published factors are against slow
  baselines or skip validation.
- **Everything good is fast enough.** By those figures a 10 MB wavetable pack is about 20 ms through yyjson and about
  11 ms through simdjson's lazy API, once, off the audio thread, and a 2 kB preset is microseconds in any of them.
  Only the slow libraries (about a quarter of a second for the same pack) would be felt.

So speed alone does not pick between the good candidates. What does: correct rounding to `float`, the same result on
every machine, safety on input from the network, memory, and how much there is to build and pin.

**Numbers: fast_float, in every option.** It is the one piece I would not do without.

- It reads text straight to a correctly rounded `float`. Reading a `double` and narrowing it rounds twice, and is
  occasionally one bit off: simdjson's documentation says a narrowed `get_double()` "is not always the float
  nearest".
- It ignores the locale. `strtod` does not, and pugixml and TinyXML-2 convert with `strtod` and `sscanf`. Inside a
  DAW on a machine set to German, `"0.5"` read through the C library is a bug waiting; and "the same bits on every
  machine" is a promise FMinus makes elsewhere.
- It is a header, about five times faster than `strtod`, and permissively licensed (Apache-2.0, MIT or BSL-1.0).

### 6.1 JSON: three ways

| | yyjson 0.13.0 (2026-09-08) | simdjson 5.1.1 (2026-11-01) | FMinus's own, on fast_float |
|---|---|---|---|
| What it is | two C89 files; one pass into a flat array of 16-byte values | two-stage SIMD parser; a tape (DOM) or a forward-only cursor (On-Demand) | three primitives over a validated buffer: skip a value, find a key, read a number or an array of them |
| License | MIT | Apache-2.0 or MIT | FMinus's |
| Code added (x86-64 object, the survey's build) | about 92 KB reader-only | about 475 KB; a 13 MB amalgamation that takes 22 s to compile | small |
| Same code on every target | yes: no SIMD, no dispatch | one kernel on arm64; four on x86-64, chosen at first use (the results are the same) | yes |
| Validation | full RFC 8259 and UTF-8; non-recursive; a depth limit to set | full; depth 1024 | **yours to write and to fuzz** |
| Floats | `double` only | `get_float()` rounds to `float` directly | fast_float |
| Memory for a 1M-float array | the text plus 16 MB of values | the text (padded) plus its index | the text |
| Random access by path, as §4.3's builtins read | natural: it is a tree | the DOM, at its lower speed; On-Demand must rescan from the top for each path | natural if the paths are prepared (§6.4) |
| Catches | pre-1.0, about a release a year; a generated tarball only | padded input, which its own docs warn can trip sanitizers (FMinus builds with them); five releases in one week | correctness on untrusted input is yours |

**I recommend yyjson's reader with fast_float for the first version**, and keeping the builtins such that the engine
behind them can change. yyjson is the least to build, behaves identically everywhere, validates fully, and fits the
tree-shaped reads §4.3 asks for. To get exact `float`s out of it, have it leave numbers as their text and convert
them with fast_float when a builtin reads them: yyjson has a read flag for exactly that
(`YYJSON_READ_NUMBER_AS_RAW`, "read all numbers as raw strings", in its header as of 0.13.0). That also takes the
conversion to `double` out of parsing, which is most of an array's cost.

simdjson is the choice if a profile ever shows array loading mattering: about twice the float throughput, and exact
`float`s built in. Its weight and its sanitizer caveat are why it is not first.

### 6.2 XML: pugixml

**Embed pugixml 1.16 (2026-06-16)**, built without XPath and without exceptions. The survey leaves little to choose:

- **It cannot be attacked through entities.** It skips a DOCTYPE and expands only the predefined and numeric
  references, so a "billion laughs" document and an external entity are both inert, by construction and not by a
  limit someone must remember to set. It fetches nothing.
- **It is small and fast:** about 102 KB without XPath, 520 to 1700 MB/s by its own benchmark, 5 to 12 times faster
  than Expat and libxml2 on its published data, with a non-recursive parse loop.
- **It is MIT, fuzzed continuously, released this year, and ships a release tarball** that pins as FMinus's other
  dependencies do.
- **Its costs:** it is lenient (it accepts some malformed XML and does not validate UTF-8), so it is a reader, not a
  validator; it has no depth limit of its own, so FMinus sets one, with a size limit; and its number conversion is
  the C library's, so FMinus takes the text and gives it to fast_float.

The alternatives each fail on something that matters here. Expat's own changelog opens with "Expat has UNFIXED
SECURITY ISSUES", six CVEs were fixed in the last two months, and its funding is stated only to January 2027.
libxml2's README says it is "NOT recommended to use this software to process untrusted data". TinyXML-2 is 6.7 times
slower with no release in 2026. RapidXML's upstream stopped in 2009.

**I would not write an XML parser.** Real files carry DOCTYPEs, CDATA, comments, processing instructions and
character references, and getting those right buys nothing that 100 KB of pugixml does not already give. SIMD XML
exists in research (Parabix, icXML, a recent Rust crate), with gains under 2x over a good scalar parser and no
production C or C++ library.

### 6.3 Limits on untrusted input

Whatever the engine: a maximum document size and nesting depth (the host's, with defaults), no recursion on the
input's depth, every read bounds-checked against the buffer, a result of "not a number" expressed as the builtin's
fallback and never as a trap, and both parsers under libFuzzer in the `asan` preset from their first commit.

### 6.4 What a compiler can do that a library cannot

The paths are literals, so the compiler sees them all. Three levels, in increasing ambition:

1. **Prepare each path when the program is compiled.** `"/osc/%d/detune"` becomes a private constant table of steps
   (a key's bytes and length, an index, a `%d` slot), and the builtin receives the table. The runtime never parses a
   path string, and a malformed one is a compile error at its own column (`'%d' in a path takes an int; 2 given, 1
   used`). This costs little and I would do it from the start, whichever engine is underneath.
2. **Treat the program's paths as the schema.** The union of the literal paths a program uses on a document is
   everything it will ever read. `json_parse` given that set can extract those values in one pass, skip everything
   else, and keep a table of slots and number arrays instead of a tree. A read is then an array load: a few
   nanoseconds, honestly wait-free, and legitimately callable on the audio thread; and the document's memory is what
   the program uses of it, not 16 bytes a value. This is the published "parse only what you need" idea (Mison,
   simdjson's On-Demand, Glaze's perfect-hashed keys) with the schema coming from the program for free. The
   literature says to expect about 2x on parse time; the memory and the audio-thread reads are the better reasons.
3. **Generate the parser itself** as code for each program. Not worth it: it runs once, off the audio thread.

Level 2 is where "FMinus's own optimized parser" makes sense, and it is an engine change behind unchanged builtins.
I would ship level 1 over yyjson and pugixml, measure real programs, and build level 2 for JSON if they ask for it.

*A measurement I could not make: I wrote a benchmark of exactly these choices on FMinus-shaped inputs (a 6.5 MB
wavetable pack, a 12 MB impulse response, a 1.7 kB preset read by 40 paths, a small array behind 5 MB of other
members; yyjson, simdjson's two APIs, pugixml, and a 150-line path extractor on fast_float). This session's
permissions do not allow compiling and running downloaded third-party code, so it never ran, and the figures above
are the libraries' and the papers' own.*

## 7. The build-time switch

Three options, as `FMINUS_BUILD_PYTHON` is one:

```cmake
option(FMINUS_JSON "The JSON parser and the json_* builtins" ON)
option(FMINUS_XML  "The XML parser and the xml_* builtins" ON)
option(FMINUS_WEB  "The web library: http_* (and ws_*) builtins; fetches and builds its dependencies" OFF)
```

| Piece | With the option on | With it off |
|---|---|---|
| Dependencies | pinned in `FMinusPins.cmake`, fetched by `cmake/FMinusWeb.cmake` (and `FMinusJson.cmake`, `FMinusXml.cmake` if a library is embedded), their own CMake never added, symbols hidden, as PFFFT's | not fetched: a default configure downloads nothing new |
| Library | `fminus_web`, a static library of its own (`src/web/`, no LLVM), as `fminus_circuit` is; `fminus_lib` links it. The parsers go into `fminus_runtime` (`src/runtime/Json.cpp`, `Xml.cpp`): they have no dependency a process could object to | no target; the runtime is as it is today |
| `fminus/Config.h` | `#define FMINUS_HAS_WEB 1` (and `_JSON`, `_XML`), so a host can `#if` | `0` |
| The compiler | the builtins' tables in `Calls.cpp`; the effects' lists | **the names stay known**, so `http_get(...)` is `error: 'http_get' needs the web library, which this build of FMinus leaves out (FMINUS_WEB=OFF)`, not "call to undeclared function" |
| A program | a predefined macro for `#if` (`#ifdef FMINUS_WEB`), beside the host's `-D`s, so one source can compile both ways | undefined |
| The JIT | `defineRuntimeSymbols` defines the web library's functions by address, from a `.def` of their own | nothing to define |
| Executables (`--emit exe`) | `linkExecutable` adds `libfminus_web.a`, its dependencies' archives and the system libraries they need (frameworks on macOS, `ws2_32` and friends on Windows), when the module calls into it | unchanged |
| Presets, CI | a `web` preset (`dev` with `FMINUS_WEB=ON`), a CI job for it on each platform | the existing jobs prove the off build stays clean |
| Tests | Catch2 tests against a loopback server the test starts (no internet, ever); lit tests marked `REQUIRES: web` (`lit.cfg.py` already has features) | skipped |
| Python, the site | unaffected: the package binds `FMinus.h`, and gains `Options::web` only if you want it to | unaffected |

What the recommendations of §5 and §6 would pin, as of 2026-11-10 (digests to be taken when the pins are written):

| Dependency | Version | License | Fetched when |
|---|---|---|---|
| fast_float | 8.3.3 (2026-10-31), the release's single `fast_float.h` | Apache-2.0, MIT or BSL-1.0 | `FMINUS_JSON` or `FMINUS_XML` |
| yyjson | 0.13.0 (2026-09-08), two C files | MIT | `FMINUS_JSON` |
| pugixml | 1.16 (2026-06-16), three files | MIT | `FMINUS_XML` |
| curl | 8.23.0 (2026-10-14) | curl | `FMINUS_WEB` |
| mbedTLS | an LTS branch: 4.1 (to at least March 2029) or 3.6 (to at least March 2027) | Apache-2.0 or GPL-2.0-or-later | `FMINUS_WEB`, on macOS and Linux |

All are compatible with FMinus's GPLv3; each adds a line to the README's "License" section, and the notices a binary
must carry, as PFFFT's does today.

Two details worth settling early:

- **Why a library of its own and not part of `fminus_runtime`.** The runtime is what every host and every executable
  links. Networking brings threads that must be joined before a plugin unloads, system libraries, and a TLS stack; a
  host that wants none of that should be able to link a FMinus that has none of it in the binary, not merely none of
  it reachable.
- **`FMINUS_WEB=ON` with `FMINUS_JSON=OFF` and `FMINUS_XML=OFF`** is legal (raw bytes still decode) and worth a
  configure-time note, since it is probably a mistake.

## 8. The host's side: permission, limits, lifetime

A build option says whether FMinus *can* reach the network. Whether a given program *may* is the host's decision
each time it compiles one, and the default must be no.

```cpp
fminus::Program::Options options;
options.web.allow = {"https://example.com/presets/*", "https://cdn.example.com/*"};   // nothing listed: no network
options.web.maxBodyBytes = 64 << 20;
options.web.timeoutMs = 15000;
options.web.cacheDirectory = "/…/Caches/MyPlugin/fminus";                              // empty: no disk cache
auto program = fminus::Program::compile(source, errors, options);
for (const auto& use : program->webRequests())      // every URL literal: its pattern, its method, its line
    show(use.pattern, use.method, use.location);
```

- **Static listing.** Because URLs are literals (§3), `webRequests()` is complete: a host can show a consent prompt,
  or refuse to run a program that names a host it does not allow, before any code runs. This is the strongest
  argument for literals-only strings while the feature is young.
- **Checked again at run time**, in the library, against the same list: after `%d` is filled in, and **after every
  redirect** (a redirect to `http://169.254.169.254/` or `http://localhost:…` is the classic way an allowed URL
  becomes a forbidden one). `https` only unless the host says otherwise.
- **Limits** on body size, redirects, time, and requests in flight, each a refusal the program reads from
  `http_status`.
- **The context.** The program's module carries one global the host sets, as it carries the loop budget: the
  program's policy and its list of open handles. `~Program` cancels what is in flight and frees every handle, so a
  program that never calls `http_close` leaks nothing past its own life, and no I/O thread is left running code of a
  plugin that has been unloaded.
- **Threads.** One I/O thread for the process, started on first use and joined when the last program with a context
  goes. Not the parallel regions' pool: those threads run at user-interactive priority and must never sit in a
  socket.
- **Plugins.** A sandboxed host (AUv3, the Mac App Store) needs the network client entitlement, which is the host's
  to have, not FMinus's; a listening socket brings a firewall prompt on macOS and Windows. An offline bounce should
  not depend on a server: the cache and an `offline` policy (serve the cache or fail) cover it.
- **Determinism.** circuits.md's promise, "the output never depends on how a host splits its blocks", cannot hold for
  a program that starts using a table on the block in which it arrived. The load-then-run shape keeps it; a program
  that polls on the audio thread gives it up, and the manual should say so in those words.

## 9. Testing without the internet

- **A loopback server in the tests,** started by the test itself on an ephemeral port: fixed responses, slow
  responses (for timeouts and cancel), redirects (to an allowed and to a forbidden address), oversize bodies, a
  dropped connection. TLS with a certificate authority the test generates and hands to the library, so no test
  depends on the machine's trust store or on a public host.
- **Parser conformance:** the JSON test suite and the XML conformance tests' well-formedness cases, plus FMinus's own
  cases (a path into every node kind, `%d`, the fallbacks, arrays longer than the buffer). Fuzzing each parser with
  libFuzzer under the `asan` preset is cheap and is where parsers earn trust.
- **Effects:** each builtin in a nonblocking and a nonallocating function, pinned as the FFT's are
  (`tests/unit/FftTest.cpp`).
- **The off build:** the message for each builtin, and that a default configure fetches nothing.

## 10. Decisions that are yours

1. **Strings:** literals as builtin arguments only (B), or C's `const char*` now (C)? I recommend B.
2. **Scope of the first version:** HTTP(S) GET and POST, off the audio thread, with JSON, XML and raw floats; no
   WebSocket, no server. Agreed?
3. **TLS:** bundled and pinned, or the platform's (§5).
4. **Parsers:** embedded libraries (yyjson, pugixml) with fast_float, or FMinus's own (§6). I recommend embedding
   first, with paths prepared by the compiler.
5. **Defaults:** `FMINUS_JSON` and `FMINUS_XML` on, `FMINUS_WEB` off.
6. **Numbers:** a JSON number read as a `float` is correctly rounded from its text; what `json_int` gives for `3e9`
   or `1.5` (the fallback, or a saturated or truncated value) needs one rule.
7. **XML indices:** from 1 as XPath, or from 0 as C.
8. **The name.** "Web" covers HTTP and WebSockets; if raw sockets or OSC over UDP are wanted later, `net` ages
   better as the option's and the library's name.

## Appendix A: the HTTP and TLS survey

From a survey made for this report on 2026-11-10. Versions and dates were read on each project's release page or
feed that day; licenses and build options in the projects' own files. Apple's developer documentation could not be
read, so what is said of NSURLSession is from memory. No upstream states added binary size or build time.

| Library | Latest release | License | HTTP client | WebSocket client | Server | TLS backends | For FMinus |
|---|---|---|---|---|---|---|---|
| libcurl | 8.23.0, 2026-10-14 | curl | yes; HTTP/2 with nghttp2 | basic: no extensions, no automatic pings | no | Schannel, mbedTLS, the OpenSSL family, wolfSSL, GnuTLS (Secure Transport and BearSSL were removed in 8.15.0) | recommended |
| cpp-httplib | 0.65.0, 2026-11-05 | MIT | HTTP/1.1 | yes, two threads a socket | HTTP and WebSocket | OpenSSL 3+, mbedTLS, wolfSSL | the fallback, and the server if one is wanted |
| Mongoose | 7.23, 2026-08-12 | GPL-2.0-only or commercial | yes | yes | yes | its own, mbedTLS, OpenSSL, wolfSSL | excluded: not combinable with GPLv3 |
| CivetWeb | 1.16, 2023-04-10 | MIT | minimal | minimal | yes | OpenSSL, mbedTLS, GnuTLS | a server first; no release in three years |
| Boost.Beast | Boost 1.92.0, 2026-08-12 | BSL-1.0 | building blocks | yes | building blocks | OpenSSL's API | a 103 MB archive; redirects, proxies and trust are yours |
| IXWebSocket | 12.1.0, 2026-10-14 | BSD-3-Clause | "hasn't received as much testing" | yes, with deflate | yes | mbedTLS, OpenSSL, LibreSSL, Apple's | WebSocket-first; the maintainer says time is short |
| libwebsockets | 5.1.0, tag 2026-10-30 | MIT and small parts | yes | yes | yes | many, Schannel among them | large; tags only; a new major line |
| uWebSockets | 20.81.0, 2026-10-26 | Apache-2.0 | no | no | yes | OpenSSL, wolfSSL | a server only |
| libhv | 1.3.4, 2025-10-25 | BSD-3-Clause | yes | yes | yes | OpenSSL, mbedTLS, GnuTLS | slow releases |
| cpr | 1.15.1, 2026-10-13 | MIT | curl's | no | no | curl's | a C++ wrapper that adds no coverage |
| WinHTTP, NSURLSession | the system's | none | yes | yes | no | the system's | no Linux; three code paths |

TLS libraries: mbedTLS 4.2.0 (2026-07-09; Apache-2.0 or GPL-2.0-or-later; a CMake build that knows when it is a
sub-project; LTS branches 4.1 to at least March 2029 and 3.6 to at least March 2027; a release tarball with a SHA-256
file). OpenSSL 4.1.0 (2026-10-23; Apache-2.0; a Perl build, no CMake). LibreSSL 4.4.0 (2026-10-22; a CMake build;
its license's compatibility with GPLv3 was not checked and must be before it is considered). AWS-LC v5.13.0 (wants
Perl and Go). wolfSSL 5.9.4 (GPLv3 or commercial; 11 CVEs fixed in that release). BearSSL 0.6 (2018; no TLS 1.3).

Root certificates with curl: Schannel reads the Windows store; on macOS curl's Keychain support
(`USE_APPLE_SECTRUST`) is a configure error with anything but OpenSSL or GnuTLS, so with mbedTLS FMinus supplies the
roots; on Linux `CURL_CA_BUNDLE=auto` bakes in the build machine's path, so an executable that runs elsewhere needs
the path set at run time.

## Appendix B: the JSON and XML survey

Made the same day, the same way. Object sizes are the survey's own build (x86-64 Linux, GCC 13.3, `-O2`, the `text`
of the unlinked object); nothing was run. Speeds are published figures: "K&L" is Keiser and Lemire, "On-Demand JSON"
(2024), its geometric mean and then its float-heavy task, in GiB/s on a Xeon Gold 6338.

| JSON | Latest release | License | Shape | Floats | Published speed | Notes |
|---|---|---|---|---|---|---|
| simdjson | 5.1.1, 2026-11-01 | Apache-2.0 or MIT | C++; On-Demand and DOM; 475 KB | `get_float()` direct to binary32 | K&L: 3.3–3.6 and 0.87–0.91 lazy; 1.9–2.1 and 0.48–0.51 DOM | padded input; runtime CPU dispatch on x86-64; fuzzed continuously |
| yyjson | 0.13.0, 2026-09-08 | MIT | C89, two files; immutable DOM; 92 KB reader-only | `double` | K&L: 0.84–1.4 and 0.45–0.47 | no SIMD; 16 bytes a value; depth limit optional; fuzzed continuously |
| Glaze | 9.1.0, 2026-10-28 | MIT | C++23 header-only; reads into C++ structs by reflection | vendors fast_float | 1200 MB/s on a 670-byte document (its own table) | needs the types at C++ compile time: not FMinus's situation |
| RapidJSON | v1.1.0, 2016; one commit in 2026 | MIT | C++03 headers; SAX and DOM | `double`; full precision is opt-in | K&L: 0.37–0.46 and 0.24–0.25 | no release in ten years |
| nlohmann/json | 3.12.0, 2025-04-11 | MIT | C++11 header; exceptions | `double` | K&L: 0.069–0.075 and 0.043–0.045 | the slow baseline |
| Boost.JSON | Boost 1.92.0 | BSL-1.0 | needs parts of Boost | | 308 MB/s (Glaze's table) | Boost |
| cJSON | 1.7.19, 2025-09-09 | MIT | C89; 19 KB; a linked list a node | `strtod` | 0.32 GB/s (yyjson's table) | does not reject invalid UTF-8 |
| sajson, jsmn, sonic-cpp | dormant, dormant, 2025 | MIT, MIT, Apache-2.0 | | | | no releases; no MSVC for sonic-cpp |
| fast_float (numbers) | 8.3.3, 2026-10-31 | Apache-2.0, MIT or BSL-1.0 | C++11 header | `float` and `double`, correctly rounded | 1042 MB/s against `strtod`'s 191 | the release ships a single `fast_float.h` |

| XML | Latest release | License | DTD and entities | Speed against pugixml | Notes |
|---|---|---|---|---|---|
| pugixml | 1.16, 2026-06-16 | MIT | DOCTYPE skipped; predefined and numeric references only | 520–1700 MB/s by its own benchmark | 102 KB without XPath; lenient; no depth limit of its own; fuzzed continuously |
| Expat | 2.10.0, 2026-11-02 | MIT | parses the internal subset and expands entities, with amplification limits; both can be compiled out | 5.8x slower, building no tree | "UNFIXED SECURITY ISSUES" in its changelog; funding stated to January 2027 |
| libxml2 | 2.15.4, 2026-09-01 | MIT | full DTD support, with limits | 5.3x (SAX) to 12x (DOM) slower | its README warns against untrusted data |
| TinyXML-2 | 11.0.0, 2025-03-15 | Zlib | not parsed | 6.7x slower | depth limit 500; `sscanf` for numbers |
| RapidXML | 1.13, 2009 | BSL-1.0 or MIT | ignored | 1.27x slower | upstream stopped; forks without releases |
| yxml | no releases; 2020 | MIT | ignored | | a 6 KB pull tokenizer with no allocation |
| Mini-XML | 4.0.6, 2026-09-22 | Apache-2.0 with exceptions | none | | no CMake build |

Papers the techniques in §6 come from: Langdale and Lemire, "Parsing Gigabytes of JSON per Second" (VLDB Journal,
2019), the two-stage SIMD design; Keiser and Lemire, "On-Demand JSON" (Software: Practice and Experience, 2024);
Lemire, "Number Parsing at a Gigabyte per Second" (2021), and Mushtak and Lemire (2023), the number reader behind
fast_float; Li and others, "Mison" (VLDB 2017), speculation on where a wanted field sits; Cameron and others on
Parabix and icXML, SIMD for XML.

## Appendix C: what was not verified

- Nothing here was built or run: no binary sizes after linking, no build times, no speeds of my own (§6).
- SHA-256 digests for the pins: to be taken when the pins are written, from the release pages.
- The Keychain calls a macOS trust loader would use (§5); what is said of NSURLSession, of the App Sandbox's network
  entitlement and of firewall prompts; and that GPL-2.0-only code cannot be combined with GPLv3 (the FSF's position):
  all from memory.
- LibreSSL's license against GPLv3; arm64 and MSVC object sizes; whether GitHub's generated tarballs keep their
  bytes (FMinus already relies on that for Catch2 and PFFFT).
- The National Vulnerability Database could not be reached, so CVE counts are the projects' own pages'.
