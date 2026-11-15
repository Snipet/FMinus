// src/runtime/Runtime.h: the FMinus runtime, the C functions generated code calls. It builds as its own static
// library, libfminus_runtime.a (FMinus::Runtime), with no LLVM in it. A process that JITs links it and hands each
// function to LLJIT by address (support/JitRuntime.h); an executable from fminus::aot links the library itself, so
// JIT'd and ahead-of-time code call the same functions. Every function here is listed in RuntimeSymbols.def.
//
// An integration piece that came with the project skeleton: the ABI version below is a placeholder for the real
// runtime. Keep at least one source in runtime/ (the library cannot be empty).
#pragma once

#include "fminus/Config.h"   // FMINUS_HAS_JSON, FMINUS_HAS_XML, FMINUS_HAS_FILES: whether this build has each part

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

    // The type string's (Text.cpp, for ir/Text.cpp): a string is a pointer and a length, the `n` bytes at `s` (a null
    // pointer or a length that is not positive is the empty string), which these read and nothing else. None
    // allocates, locks or waits, so the audio thread may call them (wait-free: each is bounded by its strings'
    // lengths).
    //   fminus_rt_string_equal   1 when the two strings have the same length and the same bytes, else 0: what == and a
    //                            switch on a string compare by; it reads no byte when the lengths differ.
    //   fminus_rt_string_find    where `w` first occurs in `s`, from 0, or -1; an empty `w` is found at 0.
    //   fminus_rt_string_float   the whole string read as a float, by the rules a query's number has (web/Server.cpp):
    //                            a decimal number as fast_float's general format reads it (no '+', no space around
    //                            it), correctly rounded, and finite (an overflow, an infinity or a NaN is no number);
    //                            else `fallback`.
    //   fminus_rt_string_int     the whole string read as an int, as a query's is: decimal digits, a '-' before them
    //                            or not, and a value an int holds; else `fallback`.
    //   fminus_rt_string_copy    the first `cap` bytes of `s` at most copied to `out` (none for a null `out` or a cap
    //                            that is not positive; the two may overlap), and the string's length, which may be
    //                            more: a call with a cap of 0 asks how many there are.
    std::int32_t fminus_rt_string_equal(const char* a, std::int32_t an, const char* b, std::int32_t bn) noexcept;
    std::int32_t fminus_rt_string_find(const char* s, std::int32_t n, const char* w, std::int32_t wn) noexcept;
    float fminus_rt_string_float(const char* s, std::int32_t n, float fallback) noexcept;
    std::int32_t fminus_rt_string_int(const char* s, std::int32_t n, std::int32_t fallback) noexcept;
    std::int32_t fminus_rt_string_copy(char* out, std::int32_t cap, const char* s, std::int32_t n) noexcept;

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

    // The string at the path (a read as the others are: wait-free, bounded by the document): the address of its bytes,
    // unescaped, as the parser holds them, and their count in `*length` (when `length` is not null); valid until the
    // document is freed. A 0 byte follows them, not counted, but a string may hold one itself ("\u0000"): its length
    // is the count. Null, and 0, when the value there is not a string, or nothing is there; an empty string is a
    // non-null address and 0.
    const char* fminus_rt_json_text(const void* document, const void* path, const std::int32_t* ints,
                                    std::int32_t count, std::int32_t* length) noexcept;

    // ---- shapes: the values in memory that json_write writes and json_module fills (JsonShapes.h) -------------------
    //
    // A shape is a constant of the program, a tree of int32 words in a table laid out as a path's (`words` int32s,
    // then `length` bytes, at 4-byte alignment, together: int32 words, length; int32 code[words]; char bytes[length]).
    // Each shape is its kind and then the kind's words:
    //   1 float            4 bytes                                     (no words)
    //   2 double           8 bytes
    //   3 int              4 bytes
    //   4 bool             1 byte, 0 for false and anything else true
    //   5 string           an address (8 bytes), then its length, an int, at 8: what a string's two slots hold
    //   6 char array       n                  n bytes, the text those before the first 0 (or all n)
    //   7 enumeration      size, n, then n triples (value, a, b): the constant `value` is named bytes[a] to
    //                      bytes[a + b - 1]; the value an int (size 4) or a byte (size 1: an enumeration of bool)
    //   8 pointer          <shape>            an address (8 bytes) of a value of that shape; null is JSON's null
    //   9 array            count, stride, <shape>   `count` elements `stride` bytes apart (a 2-D array is an array
    //                                             of arrays: the outer stride a row's bytes)
    //  10 span             capacity, stride, <shape>   an address, then a count, an int, at 8 (a pointer and a count
    //                                             in two slots), the count clamped to 0 to `capacity`
    //  11 vector           lanes              2, 4 or 8 floats (a float2, float4, float8)
    //  12 module           n, then n fields   an object: each field a record of words,
    //                          size, a, b, offset, flags, [least (2 words), greatest (2 words)], [default (2 words)],
    //                          <shape>
    //                      `size` the record's words (itself among them); the name bytes[a] to bytes[a + b - 1] (as
    //                      it is: the writer escapes it); the value at `offset` bytes from the module's address;
    //                      flags 1, a range, and 2, a default, whose numbers follow, each a double as two words (its
    //                      IEEE bits' low 32 first). A range only on a field whose values are numbers (float, double,
    //                      int, an enumeration's, or arrays of them), a default on those or a bool's.
    // Pointers, arrays, spans and modules nest at most 16 deep (the outermost shape at depth 0); a module has at most
    // 1024 fields. A table is checked whole before a byte of the program's memory is read by it, and one that is not
    // well formed (a word or a byte outside it, an unknown kind, a negative count, size or stride, a field's size not
    // its record's, too deep) is refused: nothing is read or written. The offsets, counts and strides themselves, and
    // the memory they reach, are trusted, as a compiler's constant is.
    //
    // fminus_rt_json_write writes a JSON literal's text into `buffer`, from its description (a table, as above, whose
    // code is a list of operations) and its values, the 8-byte slots at `values` that the compiled code filled:
    //   0, a, b            bytes[a] to bytes[a + b - 1], as they are (the keys and the punctuation, which the compiler
    //                      wrote and escaped)
    //   1, slot, <shape>   the value of the shape at values + 8 * slot (a scalar stored at its slot's start in its own
    //                      type; a string, a span: its address in one slot and its length in the next; a vector in as
    //                      many slots as its bytes take; an array or a module by a pointer shape, its address in the
    //                      slot)
    // A float or a double is the shortest decimal digits that read back as the same value (std::to_chars: 0.1f is
    // 0.1), NaN and the infinities null; an int in decimal; a bool true or false; an enumeration's value the name of its
    // first constant with that value, as a string, or its number when none has it; a string, and a char array, a JSON
    // string, '"' and '\' escaped and U+0000 to U+001F as \b \f \n \r \t or \u00XX, every other byte as it is
    // (no UTF-8 check); arrays, spans and vectors JSON arrays; a module an object of its fields in their order. Returns
    // the bytes the whole text takes (INT32_MAX when it is that or more), and writes the first `capacity` of them: a
    // result above the capacity did not fit, and nothing past the capacity is written (a null buffer or a capacity not
    // positive: none). -1, and nothing written, when the description is not well formed, or it has a value and
    // `values` is null. It allocates nothing, takes no lock and makes no system call (wait-free), and takes time in
    // proportion to the description and the values' sizes; it recurses as deep as its shapes nest.
    std::int32_t fminus_rt_json_write(char* buffer, std::int32_t capacity, const void* description,
                                      const void* values) noexcept;

    // fminus_rt_json_module fills the module at `module` from the object at the path, by `shape`, a table whose code is
    // one module shape (a pointer, a string or a span in it, or a char array of no byte, is not well formed). Each
    // field is filled only by a value of its kind: a float, a double or an int by the number json_float, json_double
    // or json_int reads; a bool by true or false; an enumeration by a string that is one of its names (byte for byte,
    // the first) or an integer an int holds (0 or 1 for one of bool); a char array of n by a string of at most n - 1
    // bytes (its bytes, the rest zeroed); an array or a vector by an array, as many elements as fit (more is no
    // error), each by the same rules, one of the wrong kind left as it was, and elements past the document's left as
    // they were; a module by an object, by its own fields' rules. A number outside the field's range (an array's
    // element's too) does not fill it. A field not filled is set to its default when it has one (an array's every
    // element), and a module to its own fields' defaults; else it is left as it was. A member no field names is
    // ignored; of two members of one name, the first is read. A null document, a path where nothing is, or a value
    // there that is no object fills nothing, and the defaults apply. Returns 1 when every field with no default, at
    // every depth (the modules of an array's elements that the document has among them), was filled; else 0. 0, and
    // nothing written, for a table that is not well formed or a null module. One walk of the object's members (each
    // nested object's once), each looked for among the fields its module has not yet named: steps in proportion to
    // the members times the fields, besides reading the values, as the other reads are (wait-free, bounded by the
    // document and the shape); it recurses as deep as the shape nests.
    std::int32_t fminus_rt_json_module(const void* document, const void* path, const std::int32_t* ints,
                                       std::int32_t count, const void* shape, void* module) noexcept;
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

    // The value at the path as text (a read as the others are): an attribute's value or an element's text, as the
    // other reads find it, XML's white space before and after it left out: the address of its bytes and their count in
    // `*length` (when `length` is not null), no 0 byte after them, valid until the document is freed. Null, and 0, when
    // nothing is at the path; an element with no text is a non-null address and 0.
    const char* fminus_rt_xml_text(const void* document, const void* path, const std::int32_t* ints,
                                   std::int32_t count, std::int32_t* length) noexcept;
#endif

#if FMINUS_HAS_FILES
    // The file interface (Files.cpp; a build with FMINUS_FILES only), for FMinus's file_* builtins. Files.h is its
    // host half: the policy, its check, the context, the rules a name must keep to, and the status codes
    // (FMINUS_FILE_OK 0, _MISSING -1, _REFUSED -2, _DENIED -3, _LIMIT -4, _FAILED -5, _INVALID -6), which a program
    // knows as FILE_OK, FILE_MISSING and the rest.
    //
    // A name, or a listing's pattern, is the `length` bytes at `name` (a format: each %d the next of the `count` ints
    // at `ints`, %% a percent sign; Format.h), filled in first and then checked in full (Files.h): the compiler's check
    // of the literal is a courtesy, this one the rule. An extension a %d writes is refused, as is a name of more than
    // 1024 bytes or 16 segments. The context is the program's (Context::handle); a null one is a program with no files,
    // whose every operation is FMINUS_FILE_REFUSED.
    //
    // A handle is an opaque pointer that knows its kind, a file or a listing: each function reads the kind first, and a
    // null handle, or one of the other kind, is FMINUS_FILE_INVALID, 0 or null (it is still a program's bug to pass
    // memory that was never a handle). A file that could not be read, or a listing that could not be made, is a handle
    // too, whose status says why: one of a few shared ones, which hold nothing and need no closing (closing one is
    // harmless). So these never give null.
    //
    // fminus_rt_file_read opens the file the name says under the context's policy (Files.h: its folder, each
    // directory beneath it opened in turn without following a link and compared with the denied ones, the file opened
    // without blocking and examined: a regular file of an allowed type, within maxFileBytes, with one link under
    // singleLink), reads all of it into memory (read to the limit and one byte more; allocated, never mapped), closes
    // it, and gives a handle holding its bytes until fminus_rt_file_close. Its status is FMINUS_FILE_OK, or why not:
    // _MISSING (no such file or directory), _REFUSED (the policy or the rules: Files.h), _DENIED (the system said no),
    // _LIMIT (the file's size; maxOpen, the files and listings held; maxHeldBytes, the bytes of the files held),
    // _FAILED (the read failed, or memory ran out).
    //
    // fminus_rt_file_list makes a snapshot of the regular files of one folder (or directory, anywhere) whose names fit
    // the pattern (one "*" in its file part, before its extension), each a name a program could have written, of a
    // type allowed there, and under singleLink with one link; sorted by the bytes of their names; at most maxListed
    // (more is FMINUS_FILE_LIMIT, and no listing). It takes one of maxOpen's places until closed. fminus_rt_file_count
    // is how many it holds; fminus_rt_file_read_nth(list, i) reads the i-th as fminus_rt_file_read reads a name (the
    // listing's directory as it was written, then the file's name: the same checks, made again now), and an index
    // outside 0 to count - 1, or a handle that is no listing, is a handle whose status is FMINUS_FILE_INVALID. A file
    // that has gone since the snapshot is FMINUS_FILE_MISSING; one replaced since is read as it is now.
    //
    // fminus_rt_file_close frees a file's bytes (its data's address is no longer valid) or a listing, and gives back
    // its places. Null, a shared failure's handle, or memory that is no handle: nothing. A closed handle's own memory
    // stays its context's: until the context makes a later handle in its place, it reads as FMINUS_FILE_INVALID, 0 or
    // null, and closing it again does nothing, whatever the allocator; afterwards it is that later handle, as a closed
    // file descriptor's number is. One a program never closes is freed with its context.
    //
    // The reads take a handle and read fields that never change until it is closed: none allocates, locks, waits or
    // makes a system call, so the audio thread may call them (wait-free).
    //   status  FMINUS_FILE_OK, or why not (above); FMINUS_FILE_INVALID for null or a handle that is neither kind.
    //   size    a file's bytes (0 for a file not read, a listing, null).
    //   data    a file's bytes (one 0 byte after the last, not counted), valid until it is closed; null for a file
    //           not read, a listing, null.
    //   count   a listing's files (0 for a listing not made, a file, null).
    //
    // Opening, reading, listing, writing and closing allocate and wait on the disk for a time no one can bound: not for
    // the audio thread.
    void* fminus_rt_file_read(void* context, const char* name, std::int32_t length, const std::int32_t* ints,
                              std::int32_t count) noexcept;
    std::int32_t fminus_rt_file_status(const void* handle) noexcept;
    std::int32_t fminus_rt_file_size(const void* file) noexcept;
    const void* fminus_rt_file_data(const void* file) noexcept;
    void* fminus_rt_file_list(void* context, const char* pattern, std::int32_t length, const std::int32_t* ints,
                              std::int32_t count) noexcept;
    std::int32_t fminus_rt_file_count(const void* list) noexcept;
    void* fminus_rt_file_read_nth(void* list, std::int32_t index) noexcept;
    void fminus_rt_file_close(void* handle) noexcept;

    // The i-th file's name in a listing (a read, as count is: wait-free): the entry's own name, without its directory
    // ("pad1.f32"), the address of its bytes and their count in `*length` (when `length` is not null), a 0 byte after
    // them (not counted), valid until the listing is closed. Null, and 0, for an index outside 0 to count - 1, or a
    // handle that is no listing that was made.
    const char* fminus_rt_file_name(const void* list, std::int32_t index, std::int32_t* length) noexcept;

    // fminus_rt_file_read and fminus_rt_file_list with strings: the name's format has %s places as well as %d, each %s
    // the next of the `textCount` texts, the i-th the `lengths[i]` bytes at `texts[i]` (a %s past the last: empty), and
    // joins the segment it is written in. A format with no %s is exactly the entry point without strings. With one,
    // the name filled in is checked, and one that cannot be is FMINUS_FILE_INVALID (a status the program reads): a text
    // with a negative length, or a null address and bytes; a text holding "/" or "*"; a %s in the folder (or the root
    // of an absolute name) or in the extension, or a literal whose extension is not written out; a name the rules
    // refuse (Files.h: a segment's bytes, its first and last, a device's name, 255 bytes a segment, 1024 a name, 16
    // segments). Then it is opened, or listed, as a name without strings is, the policy's refusals FMINUS_FILE_REFUSED.
    void* fminus_rt_file_read_s(void* context, const char* name, std::int32_t length, const std::int32_t* ints,
                                std::int32_t count, const char* const* texts, const std::int32_t* lengths,
                                std::int32_t textCount) noexcept;
    void* fminus_rt_file_list_s(void* context, const char* pattern, std::int32_t length, const std::int32_t* ints,
                                std::int32_t count, const char* const* texts, const std::int32_t* lengths,
                                std::int32_t textCount) noexcept;

#if FMINUS_HAS_FILES_WRITE
    // Writing (a build with FMINUS_FILES_WRITE too), in a folder whose `write` the policy set; never an absolute name.
    // Each returns a status: FMINUS_FILE_OK, or why not, as a read's, and _INVALID for a negative `bytes` (or a null
    // `data` with bytes to write), _LIMIT for more than maxFileBytes, or past maxWrittenBytes or maxFilesWritten (which
    // count what is attempted, for the context's life).
    //
    // fminus_rt_file_write writes the `bytes` bytes at `data` as the whole file, replacing it whole or not at all: a
    // temporary file beside it, made exclusively through the directory's handle, written, flushed, renamed over it,
    // and the directory flushed (Linux, macOS); any failure removes the temporary file and leaves the file as it was,
    // except that a failure to flush the directory, after the rename, is FMINUS_FILE_FAILED with the new file in place.
    // The target must be absent or a regular file (a link, a FIFO or a directory there is FMINUS_FILE_REFUSED).
    //
    // fminus_rt_file_append adds them at the end of a regular file with one link, or creates it when it is absent; its
    // size after must be within maxFileBytes. It is not atomic: a failure half way may leave part of the bytes.
    std::int32_t fminus_rt_file_write(void* context, const char* name, std::int32_t length, const std::int32_t* ints,
                                      std::int32_t count, const void* data, std::int32_t bytes) noexcept;
    std::int32_t fminus_rt_file_append(void* context, const char* name, std::int32_t length,
                                       const std::int32_t* ints, std::int32_t count, const void* data,
                                       std::int32_t bytes) noexcept;

    // The same with strings in the name, as fminus_rt_file_read_s takes them.
    std::int32_t fminus_rt_file_write_s(void* context, const char* name, std::int32_t length, const std::int32_t* ints,
                                        std::int32_t count, const char* const* texts, const std::int32_t* lengths,
                                        std::int32_t textCount, const void* data, std::int32_t bytes) noexcept;
    std::int32_t fminus_rt_file_append_s(void* context, const char* name, std::int32_t length,
                                         const std::int32_t* ints, std::int32_t count, const char* const* texts,
                                         const std::int32_t* lengths, std::int32_t textCount, const void* data,
                                         std::int32_t bytes) noexcept;

#if FMINUS_HAS_JSON
    // file_write and file_append of a JSON literal (a build with FMINUS_JSON too): the file's bytes are the text
    // fminus_rt_json_write makes of `description` and `values`, and for fminus_rt_file_append_json that text and one
    // newline after it (so that appends make JSON Lines). The name, with or without strings (null `texts` and a
    // `textCount` of 0: none, exactly as fminus_rt_file_write's), as fminus_rt_file_write_s takes it. The text is
    // measured first (a call with no buffer, which writes nothing), and its size meets the same checks as `bytes`
    // does, in the same order, after the name's: more than maxFileBytes is FMINUS_FILE_LIMIT. Only then is it written,
    // once, into a buffer of exactly its size, which the file is written from as fminus_rt_file_write writes `data`;
    // the values are read twice, and must not change between (a second text of another size is FMINUS_FILE_FAILED,
    // and nothing is written). A description that is not well formed (or one with values and a null `values`) is
    // FMINUS_FILE_INVALID, as a negative `bytes` is: after the name's own refusals, before the policy's; a buffer that
    // cannot be had, FMINUS_FILE_FAILED. The same statuses, and the same quotas, as fminus_rt_file_write's otherwise.
    std::int32_t fminus_rt_file_write_json(void* context, const char* name, std::int32_t length,
                                           const std::int32_t* ints, std::int32_t count, const char* const* texts,
                                           const std::int32_t* lengths, std::int32_t textCount,
                                           const void* description, const void* values) noexcept;
    std::int32_t fminus_rt_file_append_json(void* context, const char* name, std::int32_t length,
                                            const std::int32_t* ints, std::int32_t count, const char* const* texts,
                                            const std::int32_t* lengths, std::int32_t textCount,
                                            const void* description, const void* values) noexcept;
#endif
#endif

    // An executable's (a C main whose program calls a file builtin): it has no host, so the person who runs it says
    // what it may reach, in its environment: FMINUS_FILES_READ and FMINUS_FILES_WRITE, folders as "name=directory"
    // entries separated by newlines or the system's list separator (':', ';' on Windows), read-only and writable
    // (writable: a build with FMINUS_FILES_WRITE); FMINUS_FILES_DENY, denied directories, separated the same way;
    // FMINUS_FILES_TYPES, extensions separated by spaces, commas or semicolons; FMINUS_FILES_ANYWHERE, "1" or "0" (or
    // empty). The policy's limits are its defaults. Makes the context, when a folder or anywhere is given, and stores
    // its handle in `*context` (null for no files, the default) and returns 0; or, for a mistake, says why on stderr
    // ("<program>: FMINUS_FILES_READ: ...", the variable the mistake is in) and returns 2. A JIT never defines these
    // two (RuntimeSymbols.def).
    std::int32_t fminus_rt_files_main_start(void** context, const char* program) noexcept;

    // Destroys the context fminus_rt_files_main_start made (null: nothing), after the program's main has returned.
    void fminus_rt_files_main_stop(void* context) noexcept;
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
