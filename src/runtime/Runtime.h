// src/runtime/Runtime.h: the FMinus runtime, the C functions generated code calls. It builds as its own static
// library, libfminus_runtime.a (FMinus::Runtime), with no LLVM in it. A process that JITs links it and hands each
// function to LLJIT by address (support/JitRuntime.h); an executable from fminus::aot links the library itself, so
// JIT'd and ahead-of-time code call the same functions. Every function here is listed in RuntimeSymbols.def.
//
// An integration piece that came with the project skeleton: the ABI version below is a placeholder for the real
// runtime. Keep at least one source in runtime/ (the library cannot be empty).
#pragma once

#include "fminus/Config.h"   // FMINUS_HAS_JSON, FMINUS_HAS_XML: whether this build has each parser

#include <cstdint>

// Four floats in one vector register, as FMinus's float4 is (LLVM's <4 x float>): GCC's and clang's vector of 16 bytes,
// which the C ABI passes and returns in one register on arm64 and x86-64, as it does clang's ext_vector_type(4).
typedef float fminus_rt_float4 __attribute__((vector_size(16)));

// A parallel region's body (ir/Parallel.cpp): runs its iterations, or its statements, begin to end - 1, over what its
// context holds.
typedef void (*fminus_rt_task)(void* context, std::int32_t begin, std::int32_t end);

// A program's loop budget, fminus_rt_loop_budget, which a program compiled with it defines (ir::Options::loopBudget,
// opt/LoopBudget.h) and its code counts down: one at each branch back to the start of a loop, and one at each call of a
// function that can call itself again. When `left` would go below zero, the function that took it there writes -1 to
// it, sets `stopped` and returns early, and so does each function of the program that called it, back to the host.
// It starts unlimited (INT64_MAX, and 0); the host sets `left` (to -1 or more: the program's counts rely on it never
// holding less, and -1 stops a call at its first check) and clears `stopped` before a call, and reads them after it,
// with relaxed atomics (Program::setLoopBudget, include/fminus/FMinus.h; an executable's C main, support/Aot.h). Every
// access the program makes is a relaxed atomic too, so a parallel region's threads may share it.
struct fminus_rt_loop_budget_state
{
    std::int64_t left;      // what the program may still take: -1 once it has run out, never less
    std::int32_t stopped;   // nonzero once a call has run out and returned early
};

// A program's record of a failed bounds check, fminus_rt_bounds, which a program compiled with the checks defines
// (ir::Options::boundsCheck, opt/BoundsCheck.h) and its code writes: when an access is outside its array, the function
// that makes it records the check's number and the index, unless `site` holds one already (the first is kept), and
// returns early, and so does each function of the program that called it, back to the host. It starts empty (0, 0, 0);
// the host reads it after a call, clears `site` before the next, and finds what the number means in the program's
// description of its checks (Program::boundsViolation, include/fminus/FMinus.h; an executable's C main, support/Aot.h),
// with relaxed atomics, as the program's own accesses are, so a parallel region's threads may share it.
struct fminus_rt_bounds_state
{
    std::int32_t site;       // the failed check's number, from 1, or 0 for none
    std::int32_t reserved;   // 0
    std::int64_t index;      // the element the access reached, from the array's first
};

extern "C"
{
    // The runtime's ABI version, for generated code to check against the one it was compiled for.
    std::int32_t fminus_rt_abi_version() noexcept;

    // An executable's start (fminus::aot::addEntryPoint), before it calls the program's main. Reads main's `count`
    // arguments from the command line into `args`, each as C's strtof reads it, and returns 0; or tells stderr what is
    // wrong (the wrong number of arguments, or one that is not a number) and returns 2, the executable's exit status.
    std::int32_t fminus_rt_main_args(std::int32_t argc, char** argv, std::int32_t count, float* args) noexcept;

    // The same for a main with a double among its parameters: reads each of its `count` arguments as `kinds` says, a
    // character a parameter: 'd' as C's strtod reads a double, 'f' as strtof reads a float (stored widened, exactly,
    // which the entry rounds back), into `args`.
    std::int32_t fminus_rt_main_double_args(std::int32_t argc, char** argv, std::int32_t count, const char* kinds,
                                            double* args) noexcept;

    // An executable's end: prints main's result and a newline on stdout as `fminus --run` prints a result, the
    // shortest decimal that reads back as the same float.
    void fminus_rt_main_result(float result) noexcept;

    // The same for a main that returns double: the shortest decimal that reads back as the same double.
    void fminus_rt_main_double_result(double result) noexcept;

    // The same for a main that returns bool: prints true or false.
    void fminus_rt_main_bool_result(bool result) noexcept;

    // An executable compiled with the loop budget (fminus --emit exe --loop-budget n), when main ran past it: tells
    // stderr that main returned early, before the executable exits with 3.
    void fminus_rt_main_loop_budget(std::int32_t argc, char** argv, std::int64_t budget) noexcept;

    // An executable compiled with bounds checks (fminus --emit exe --bounds-check), when main failed one: tells stderr
    // where, `before`, the index and `after` (opt::boundsMessage's parts), and that main returned early, before the
    // executable exits with 4.
    void fminus_rt_main_bounds(std::int32_t argc, char** argv, const char* before, std::int64_t index,
                               const char* after) noexcept;

    // The heap, for FMinus's malloc, calloc and free: count * size bytes (malloc: 1 * size), aligned to 64 bytes (a
    // cache line, and the alignment of the widest vector, AVX-512's) and zeroed, as every variable FMinus declares
    // starts at 0, and touched, so no page faults in later. Null when the count or the size is not positive, the
    // product is more than 16 GiB (a mistake: zeroing it would take the machine's memory), or the memory is not there.
    // The memory comes from C's aligned_alloc, so C's free releases it too, and fminus_rt_free releases C's malloc's.
    // On Windows, whose C runtime has no aligned_alloc, it comes from _aligned_malloc: only _aligned_free (which
    // fminus_rt_free calls there) releases it, and fminus_rt_free cannot release malloc's.
    // Not for the audio thread: allocating takes a lock in the system's allocator, and can take a page fault.
    void* fminus_rt_malloc(std::int64_t size) noexcept;
    void* fminus_rt_calloc(std::int64_t count, std::int64_t size) noexcept;
    void fminus_rt_free(void* memory) noexcept;

    // The FFT (Fft.cpp), for FMinus's fft_create, fft_create_complex, fft_forward, fft_inverse and fft_destroy. A plan
    // is for transforms of n points: real (n floats into n/2 + 1 complex bins, each a (re, im) pair of floats, and
    // back) or complex (n complex values into n, and back). Neither direction scales, so inverse(forward(x)) is n times
    // x. Making a plan allocates (its tables, and buffers for the transforms) and takes time: not for the audio thread.
    // It is null when n is not a size the library takes. A transform allocates nothing, takes no lock, reads `in` (n
    // floats, n/2 + 1 or n complex values, as the plan and the direction say) and writes `out`, which must not overlap
    // it; a null plan, or a plan of the other kind, does nothing.
    void* fminus_rt_fft_create(std::int32_t n) noexcept;
    void* fminus_rt_fft_create_complex(std::int32_t n) noexcept;
    void fminus_rt_fft_destroy(void* plan) noexcept;
    void fminus_rt_fft_forward_real(void* plan, const float* in, float* out) noexcept;
    void fminus_rt_fft_inverse_real(void* plan, const float* in, float* out) noexcept;
    void fminus_rt_fft_forward_complex(void* plan, const float* in, float* out) noexcept;
    void fminus_rt_fft_inverse_complex(void* plan, const float* in, float* out) noexcept;

    // Bytes to numbers (Bytes.cpp), for FMinus's bytes_floats and bytes_pcm16: `bytes` bytes at `data`, read as
    // little-endian IEEE float32s (bytes_floats), or as little-endian signed 16-bit PCM samples, each scaled by 1/32768
    // to [-1, 1) (bytes_pcm16), whatever the machine's own byte order. Each returns how many values the data holds
    // (bytes / 4, or bytes / 2: a partial value at the end is none), and writes the first of them to `out`, at most
    // `cap`: so a call with a cap of 0 asks how many there are. A null `data` or a negative `bytes` is no data, and a
    // null `out` or a cap that is not positive takes none. A float's bits are copied as they are, a NaN's payload and a
    // subnormal too. Each may decode in place, `out` at `data`'s own address (bytes_pcm16 writes its last value
    // first). Each reads `data` and writes `out` and nothing else; neither allocates, locks nor waits, so the audio
    // thread may call them (wait-free: a loop over the values, bounded by `cap`).
    std::int32_t fminus_rt_bytes_floats(const void* data, std::int32_t bytes, float* out, std::int32_t cap) noexcept;
    std::int32_t fminus_rt_bytes_pcm16(const void* data, std::int32_t bytes, float* out, std::int32_t cap) noexcept;

    // Scaffolding for string literals as builtins' arguments (ir/Strings.cpp), for FMinus's __fminus_string_hash: the
    // 32-bit FNV-1a hash of the text a format and its ints make (Format.h: each %d the next int in decimal, %% a
    // percent sign), `length` bytes at `text` (a NUL among them is a byte like any other) and `count` ints at `ints`.
    // A null `text` or a negative `length` is the empty text. It reads its arguments and nothing else, allocates
    // nothing and waits for nothing. It goes with the scaffolding's builtins (ir/Calls.cpp), when a builtin that blocks
    // (http_wait) comes with tests of its own.
    std::int32_t fminus_rt_string_hash(const char* text, std::int32_t length, const std::int32_t* ints,
                                       std::int32_t count) noexcept;

#if FMINUS_HAS_JSON
    // The JSON parser (Json.cpp, on yyjson; a build with FMINUS_JSON only), for FMinus's json_* builtins.
    //
    // fminus_rt_json_parse reads the `bytes` bytes at `data` as one JSON text (RFC 8259: a value, its strings' UTF-8
    // checked, nothing but whitespace after it; no comments, no trailing commas, no byte order mark), and gives the
    // parsed document, an opaque pointer, or null when they are no JSON text, when they are more than
    // FMINUS_JSON_MAX_BYTES (fminus/Config.h), when its arrays and objects nest deeper than FMINUS_JSON_MAX_DEPTH, the
    // outermost counted, or when the memory is not there. A null `data` or a `bytes` that is not positive is no text.
    // The document holds a copy of what it needs: `data` may change or go once it returns. It allocates (the document,
    // about 16 bytes a value, and the text's copy) and takes time in proportion to the text, so it is not for the audio
    // thread, and neither is fminus_rt_json_free, which frees a document (null: nothing).
    //
    // The reads take a document (null is an empty one: nothing is at any path), a path prepared when the program was
    // compiled (below), and the `count` ints at `ints` its places take, and touch nothing but those, the document, and
    // what they write: none allocates, locks, waits or makes a system call, and each takes steps bounded by the
    // document's size, so the audio thread may call them. Wait-free is not the same as cheap: the bound is the
    // document, not a constant. A step into an object looks through its members, and one into an array of arrays or
    // objects steps over the elements before its index (into an array of numbers, strings and the like it goes
    // straight there); a value is read whole, every digit of a number and every character of a base64 string. So one
    // read of a document a server made large can take milliseconds (a 60 MB number, tens of them), and a loop of
    // reads by %d over an object's n members takes time in n^2: a program reads a document in its init, into arrays.
    //   json_float, json_double  the number at the path, correctly rounded to a float (straight from its digits, never
    //                            through a double) or to a double; one past the type's range is an infinity of its
    //                            sign, and one nearer zero than its least subnormal a zero. Else `fallback`.
    //   json_int                 the number at the path when it is an integer an int holds (Numbers.h, readInt: 7.0
    //                            is 7, 1.5 is none and 3e9 none); else `fallback`.
    //   json_bool                1 for true, 0 for false; else `fallback`.
    //   json_count               an array's elements or an object's members, else 0.
    //   json_floats              the numbers of an array of numbers, or of an array of arrays of numbers row after row
    //                            (rows of any lengths): how many there are, each written to `out` as json_float reads
    //                            it, the first `cap` of them (a null `out` or a `cap` that is not positive takes none).
    //                            0, and nothing written, when the value is no such array, or any element is not a
    //                            number (or a row of them).
    //   json_choice              which of the choices (below) a string at the path equals, byte for byte: its index,
    //                            from 0, the first that does; else -1.
    //   json_base64_floats       the bytes a string at the path holds in base64 (RFC 4648's alphabet, with its '='
    //                            padding or without it), as fminus_rt_bytes_floats reads them, and with the same
    //                            result, without allocating: little-endian float32s, how many, the first `cap`
    //                            written. 0, and nothing written, when the value is not a string, or is not base64.
    //
    // A path is a JSON Pointer (RFC 6901), prepared from its literal when the program was compiled (ir/Paths.cpp): the
    // runtime never reads a path's text. It is `words` int32s and then `length` bytes, at 4-byte alignment, together:
    //     int32 words, length; int32 code[words]; char bytes[length];
    // `code` is the reference tokens, the steps from the document's root, in order: each is its count of pieces, n, and
    // then n pairs (a, b), the token's text being the pieces' in order: bytes[a] to bytes[a + b - 1] when b >= 0 (the
    // literal's own, ~0 and ~1 already '~' and '/'), and when b is -1 the int ints[a] in decimal (a minus before a
    // negative one). A step into an object takes the first member whose name is the token's text; one into an array
    // the element the text numbers, when it is an array index as RFC 6901 writes one (0, or digits from 1 without a
    // leading 0: never a minus) and there is such an element. Else, and when the code is not well formed (a piece
    // outside `bytes` or `ints`, a count past `words`), nothing is at the path.
    //
    // A choice table, json_choice's literals, prepared as a path is: `count` lengths and then the literals' bytes, one
    // after another, at 4-byte alignment, together:
    //     int32 count, length; int32 lengths[count]; char bytes[length];
    void* fminus_rt_json_parse(const void* data, std::int32_t bytes) noexcept;
    void fminus_rt_json_free(void* document) noexcept;
    float fminus_rt_json_float(const void* document, const void* path, const std::int32_t* ints, std::int32_t count,
                               float fallback) noexcept;
    double fminus_rt_json_double(const void* document, const void* path, const std::int32_t* ints, std::int32_t count,
                                 double fallback) noexcept;
    std::int32_t fminus_rt_json_int(const void* document, const void* path, const std::int32_t* ints,
                                    std::int32_t count, std::int32_t fallback) noexcept;
    std::int32_t fminus_rt_json_bool(const void* document, const void* path, const std::int32_t* ints,
                                     std::int32_t count, std::int32_t fallback) noexcept;
    std::int32_t fminus_rt_json_count(const void* document, const void* path, const std::int32_t* ints,
                                      std::int32_t count) noexcept;
    std::int32_t fminus_rt_json_floats(const void* document, const void* path, const std::int32_t* ints,
                                       std::int32_t count, float* out, std::int32_t cap) noexcept;
    std::int32_t fminus_rt_json_choice(const void* document, const void* path, const std::int32_t* ints,
                                       std::int32_t count, const void* choices) noexcept;
    std::int32_t fminus_rt_json_base64_floats(const void* document, const void* path, const std::int32_t* ints,
                                              std::int32_t count, float* out, std::int32_t cap) noexcept;
#endif

#if FMINUS_HAS_XML
    // The XML parser (Xml.cpp, on pugixml; a build with FMINUS_XML only), for FMinus's xml_* builtins, made as the
    // JSON parser's are: the same documents, null and not, the same fallbacks, the same promise of the reads.
    //
    // fminus_rt_xml_parse reads the `bytes` bytes at `data` as one XML document in UTF-8 (a byte order mark before it
    // is skipped), and gives the parsed document, an opaque pointer, or null when they are no XML document (malformed,
    // or no root element, or more than one), when they are more than FMINUS_XML_MAX_BYTES (fminus/Config.h), when its
    // elements nest deeper than FMINUS_XML_MAX_DEPTH, the root element at depth 1, or when the memory is not there. A
    // null `data` or a `bytes` that is not positive is no document. It is a reader, not a validator: what XML forbids
    // and does no harm (invalid UTF-8, a name used twice for attributes, text after the root element) may parse, but a
    // character reference to U+0000 in a value (&#0;), which would end it early, is no document. Its
    // DOCTYPE is read only to be skipped: the entities a document declares are never expanded, and an external one
    // never fetched; a reference to one stays in the text as it is written ("&x;"). XML's five predefined references
    // (&lt; &gt; &amp; &apos; &quot;) and character references (&#65; &#x41;) are expanded; CDATA sections are text;
    // comments and processing instructions are dropped, and the text either side of one is one text. The document
    // holds a copy of what it needs: `data` may change or go once it returns. It allocates (the document, a few dozen
    // bytes a node, and the text's copy) and takes time in proportion to the text, so it is not for the audio thread,
    // and neither is fminus_rt_xml_free, which frees a document (null: nothing). The depth is checked once the tree is
    // built, so a text past it costs its tree before it is refused: the most is about 1.5 GB, for 64 MiB of unclosed
    // "<a>"s (64 MiB of "<a/>"s, which parse, about 1.2 GB).
    //
    // The reads take a document (null is an empty one: nothing is at any path), a path prepared when the program was
    // compiled (below), and the `count` ints at `ints` its places take, and touch nothing but those, the document, and
    // what they write: none allocates, locks, waits or makes a system call, and each takes steps bounded by the
    // document's size, so the audio thread may call them (wait-free, which is not the same as cheap, as the JSON
    // reads are not: an element is found by looking through its parent's children, an attribute through its
    // element's, and a value, its length too, is read whole; a loop of reads by %d over n siblings takes time in
    // n^2). A path names an element or an attribute; the value read is the
    // attribute's value, or the element's text: its first text child, a CDATA section's or not (none: the empty
    // text). XML's white space (space, tab, line feed, carriage return) before and after a value is not part of it.
    //   xml_float, xml_double  the value as a number (XML Schema's syntax: Numbers.h, NumberSyntax::Xml), correctly
    //                          rounded to a float (straight from its digits) or to a double, as json_float's and
    //                          json_double's are. Else `fallback`.
    //   xml_int                the value when it is an integer an int holds (Numbers.h, readInt: "+007" and "7.0" are
    //                          7, "1.5" none and "3e9" none); else `fallback`.
    //   xml_bool               1 for "true" or "1", 0 for "false" or "0" (XML Schema's boolean); else `fallback`.
    //   xml_count              how many elements the path's last step matches: its parent's children of that name, or
    //                          with an index, 1 when there is one; for an attribute, 1 when it is there. Else 0.
    //   xml_floats             the value as a list of numbers, each as xml_float reads it, separated by white space
    //                          or a comma (and white space around it): how many there are, the first `cap` written to
    //                          `out` (a null `out` or a `cap` that is not positive takes none). 0, and nothing
    //                          written, when anything in it is not a number (an empty item, a comma at either end).
    //   xml_choice             which of the choices (as json_choice's) the value equals, byte for byte: its index,
    //                          from 0, the first that does; else -1.
    //
    // A path is a small part of XPath's, prepared from its literal when the program was compiled (ir/Paths.cpp,
    // scanXmlPath): child steps by element name from the root ("/preset/filter"), the first naming the root element,
    // each optionally with an index ("[2]"); and a last step that may be an attribute ("@cutoff"). A name matches the
    // name as written, a prefix and its colon among it ("svg:path"): there are no namespaces. An index counts the
    // parent's children of that name from 0, as C's arrays and JSON Pointer's do (XPath's count from 1); a step with no
    // index takes the first. The table is laid out as a JSON path's, `words` int32s and then `length` bytes, at 4-byte
    // alignment, together:
    //     int32 words, length; int32 code[words]; char bytes[length];
    // `code` is the steps in order, each: its kind, 0 an element, 1 an element and its index, 2 an element and the
    // index an int gives, 3 an attribute; then the name, as a JSON path's token is: its count of pieces, n, and n pairs
    // (a, b), bytes[a] to bytes[a + b - 1] when b >= 0 and the int ints[a] in decimal when b is -1; then for 1 the
    // index, and for 2 its place, a, the index being ints[a]. When the code is not well formed (a piece outside `bytes`
    // or `ints`, a count past `words`, an attribute before the last step, an unknown kind), or an index is negative,
    // nothing is at the path.
    void* fminus_rt_xml_parse(const void* data, std::int32_t bytes) noexcept;
    void fminus_rt_xml_free(void* document) noexcept;
    float fminus_rt_xml_float(const void* document, const void* path, const std::int32_t* ints, std::int32_t count,
                              float fallback) noexcept;
    double fminus_rt_xml_double(const void* document, const void* path, const std::int32_t* ints, std::int32_t count,
                                double fallback) noexcept;
    std::int32_t fminus_rt_xml_int(const void* document, const void* path, const std::int32_t* ints, std::int32_t count,
                                   std::int32_t fallback) noexcept;
    std::int32_t fminus_rt_xml_bool(const void* document, const void* path, const std::int32_t* ints,
                                    std::int32_t count, std::int32_t fallback) noexcept;
    std::int32_t fminus_rt_xml_count(const void* document, const void* path, const std::int32_t* ints,
                                     std::int32_t count) noexcept;
    std::int32_t fminus_rt_xml_floats(const void* document, const void* path, const std::int32_t* ints,
                                      std::int32_t count, float* out, std::int32_t cap) noexcept;
    std::int32_t fminus_rt_xml_choice(const void* document, const void* path, const std::int32_t* ints,
                                      std::int32_t count, const void* choices) noexcept;
#endif

    // The math builtins sinf, cosf, tanf, expf, tanhf and logf (Math.cpp), as the code generator's math modes call
    // them (ir::Options::math, ir/Math.cpp). None allocates, locks or makes a system call; each is a function of its
    // argument alone, which the code generator tells LLVM (it reads no memory and is defined for every argument).
    //
    // The vector mode's, FMinus's default: FMinus's own kernels (MathKernels.h), the same IEEE operations on every
    // target. A lane of a four-lane function is the scalar function of that lane, bit for bit, whatever the other lanes
    // hold: so a float gets one result in a scalar loop and in its vectorised body, in a float4, and in a poly float of
    // any width, on arm64 and on x86-64.
    float fminus_rt_sinf(float x) noexcept;
    float fminus_rt_cosf(float x) noexcept;
    float fminus_rt_expf(float x) noexcept;
    float fminus_rt_tanhf(float x) noexcept;
    float fminus_rt_logf(float x) noexcept;
    float fminus_rt_tanf(float x) noexcept;
    fminus_rt_float4 fminus_rt_sin_f4(fminus_rt_float4 x) noexcept;
    fminus_rt_float4 fminus_rt_cos_f4(fminus_rt_float4 x) noexcept;
    fminus_rt_float4 fminus_rt_exp_f4(fminus_rt_float4 x) noexcept;
    fminus_rt_float4 fminus_rt_tanh_f4(fminus_rt_float4 x) noexcept;
    fminus_rt_float4 fminus_rt_log_f4(fminus_rt_float4 x) noexcept;
    fminus_rt_float4 fminus_rt_tan_f4(fminus_rt_float4 x) noexcept;

    // FMinus's parallel for and parallel { } (Parallel.cpp): runs task(context, ...) over the iterations begin to
    // end - 1, spread over the pool's threads and the calling thread, in chunks each thread takes as it finishes its
    // last, and returns when every one has run. `threads` is the most threads that take part, the caller's among them;
    // 0 is all there are. Every chunk runs in the caller's floating-point mode (FPCR on arm64, MXCSR on x86-64). It runs
    // the whole range on the calling thread, in order, when the pool is not started, when `threads` is 1, for one
    // iteration, inside a chunk of another region the pool runs, and when another region holds the pool (a host's
    // other audio thread's). It allocates nothing and takes no lock, so the audio thread may call it: at the end it
    // spins for the chunks other threads are still running, and pool threads asleep (no region for 50 us) it wakes
    // with a system call that does not block.
    void fminus_rt_parallel_for(fminus_rt_task task, void* context, std::int32_t begin, std::int32_t end,
                                std::int32_t threads) noexcept;

    // The pool, started, or one more user of it counted: a thread for every core but one, asleep until a region
    // needs it. Each start needs its stop. Not for the audio thread: it makes threads. Returns how many threads a
    // region can run on, the caller's among them (1 on a machine of one core).
    std::int32_t fminus_rt_parallel_start() noexcept;

    // A start's end. The last stops the pool's threads and waits for them: no region may be running then.
    void fminus_rt_parallel_stop() noexcept;

    // The libm mode's: libm's sinf, cosf, ..., which C++'s std::sin of a float calls, under names LLVM does not know,
    // so that it neither folds nor vectorises them.
    float fminus_rt_libm_sinf(float x) noexcept;
    float fminus_rt_libm_cosf(float x) noexcept;
    float fminus_rt_libm_expf(float x) noexcept;
    float fminus_rt_libm_tanhf(float x) noexcept;
    float fminus_rt_libm_logf(float x) noexcept;
    float fminus_rt_libm_tanf(float x) noexcept;
}
