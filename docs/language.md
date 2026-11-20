# The language

FMinus v0 is a subset of C: C's syntax, and C's semantics where it has them.

```c
/* A soft clipper, in FMinus v0. */
float half(float x);                     // a prototype: C needs one to call half before its definition (FMinus doesn't)

float softclip(float x, const float drive)
{
    const float y = x * drive;           // const: never assigned
    float out = y / (1.0f + fabsf(y));   // <math.h>'s float functions are builtins
    out *= 0.5f;                         // locals and parameters are variables, as in C
    return out;
}

float chain(float x)
{
    return softclip(half(x), 4.0f);      // a function may be called before it is defined
}

float half(float x) { x /= 2; return x; }
```

## At a glance

| | |
|---|---|
| program | function definitions, `float name(float a, const float b) { ... }` or `void name(...)`, and prototypes, `float name(float);`; `f(void)` is C's empty list; after the parameters, clang's `[[clang::nonallocating]]` or `[[clang::nonblocking]]`, checked: the function may not reach the heap (nor, nonblocking, an external function not marked so); modules, `module OnePole { float y; float tick(float x) { return y += x; } };`, whose fields are declarations without initialisers and whose methods are functions that see the fields by name (`this` is the module's address); a module's parameters, `param float cutoff [[fminus::range(20, 20000)]] [[fminus::default(1000)]];` (a `float`, `int` or `bool`), which the host sets and a method reads as `cutoff[i]`, its value at sample i of the call; enumerations and constants (below) |
| declarations | `float x = e;`, `const float k = e;`, `float* p = out;`, several at once (`float a = 1, b;`; in `float *p, x;` only `p` is a pointer, as in C); arrays, `float h[16];` and `int k[3] = {1, 2, 3};` (the elements left out are 0), `float buf[ROWS * COLS];` (a size is C's integer constant expression: an enumeration constant names one, a `const int` does not) and `float t[] = {1, 2, 3};` (its list's length), and a `const` one of constants, `const float t[4] = {1, 2, 3, 4};`, stored once (up to 1 MiB), a constant of the program that no call fills; 2-D arrays, `float grid[4][16];`, rows side by side (C's arrays of arrays), and their lists, rows braced or not, as C places them (`float g[2][3] = {{1, 2}, {3}};`, `int k[][2] = {1, 2, 3};`, two rows), the same everywhere a 1-D array goes; at the top level, constants, C's file-scope const objects: `const float table[ROWS * COLS] = {...};` and `const float gain = 0.5f;`, their initialisers constants (numbers, enumeration constants, `sizeof`, other constant scalars of the top level, `hash` and `randf` of them, an element's address, operators, casts, `?:`, `&&` and `\|\|`), each stored once (up to 1 MiB), which every function and method declared after it reads (an `#include`d file's after the `#include`); modules, `OnePole lp;` and `OnePole b = a;` (a copy); block-scoped, shadowing allowed in an inner block; C's enumerations, `enum Wave { SAW, SQUARE = 4, TRIANGLE };` (`SAW` is 0, `TRIANGLE` 5, each an `int` constant), declared at the top level (the functions and modules after it see its constants), in a module (its methods too) or in a block, a name each in the scope a variable would have, and the names of its type after its list, as C declares them (`enum Wave { SAW, SQUARE } wave;`: a local, a field, or a constant at the top level); C23's fixed underlying type, `enum Wave : int { ... }` or `: bool`, whose constants must fit it, and which declares the tag ahead of its constants too (`enum Wave : int;`) |
| statements | `{ ... }`, `e;`, `return e;` and `return;`, `;`, `if (c) s else s` (an `else` closes the nearest `if`), `while (c) s`, `for (init; c; step) s` (its declarations are the loop's), `break;`, `continue;`, `switch (v) { case 1: s ... default: s }` (C's: an `int` or `bool` value, its cases integer constant expressions, which fall through to the next until a `break`; `case` and `default` are C23's labels, so a declaration may follow one), and a switch on a `string`, `switch (s) { case "saw": ... }`, its cases string literals, the first whose bytes equal the value's taken (WG14 N3895's, with lengths for NULs); C2y's declaration in an `if`'s or a `switch`'s head, `if (int n = count(); n > 0) s else s` and `if (json_doc doc = json_parse(data, bytes)) s` (whose condition is `doc`), its names in scope to the end of the whole statement, the `else` too; `defer s` (or `_Defer s`, ISO/IEC TS 25755's), whose statement runs when control leaves the block the `defer` is in, at its end or by `return`, `break` or `continue`, the last reached first (a `return`'s value is computed before); FMinus's `when (c) s`, which runs `s` each time `c` becomes true (true now, and false the last time this `when` was reached, or never reached: `if (c && !was) s; was = c;`), its state a `bool` of the method's module instance, so a method's statement, which in a loop over samples fires on each rising edge; a `while` or `for` whose condition is not a constant expression may be assumed to end, as in C11 (6.8.5p6), and `while (1)` and `for (;;)` may run for ever; FMinus's `parallel for (int i = first; i < end; ++i) s`, its iterations at once over the machine's cores, and `parallel { s1 s2 ... }`, its statements at once, each finished, all of it, where it ends (below); FMinus's `serve (server) { route "POST /note/{n}" (int n [[fminus::range(0, 127)]]) { ... reply 204; } ... }`, the requests waiting for its routes, each route's parameters filled from the path's holes (a number, or a `string`, a whole segment: `route "PUT /presets/{name}" (string name)`) and the query and checked (404, 400), and `reply`, which answers and leaves the route, a JSON literal `json { "gain": gain, "name": name }` among its forms, in a build with the web server (below); FMinus's `within (ms) { ... }`, a deadline for the requests its block starts: where it ends, every request the thread started in it (or in a function it called) has ended, those still pending at the deadline cancelled (`HTTP_TIMEOUT`), and every wait in it returns by the deadline, in a build with the web library (below) |
| expressions | constants, names, calls, `( )`, unary `-` `+` `!` `~`, `++` and `--` (before or after), casts `(int)x` `(float)x` `(double)x` `(bool)x`, `* / %`, `+ -`, `<< >>`, `< <= > >=`, `== !=`, `&`, `^`, `\|` (the bitwise ones on `int`s), `&&`, `\|\|` (both short-circuit), `?:` (only the branch taken runs), and `= += -= *= /= %= &= \|= ^= <<= >>=` (right to left; an assignment's value is the one stored), with C's precedence; pointers' `p[i]`, `*p`, `&x`, `p + i`, `p - q` and comparisons; an array's `a[i]` (its name stands for a pointer to its first element), and a 2-D array's `g[i][j]` and its rows, `g[i]`, each a 1-D array, a `float*` wherever one goes; a module's fields `m.y` and `p->y`, its methods `m.tick(x)` and `p->tick(x)`, and `this`; `sizeof(type)`, an `int` (a type past the greatest `int`, a module over 2 GB, is an error), arrays' types too, `sizeof(float[4][16])`; vector literals, OpenCL C's `(float4)(a, b, c, d)` and C's `(float4){a, b}` (the lanes left out are 0), and swizzles `v.wzyx`, `v.s0`; C's string literals, a `string` where one is wanted (an initialiser, an assignment, an argument, a result, an operand of `==` and `!=`, a `case`) and a builtin's literal argument (below); `==` and `!=` of two strings, by content; a `char` array's `name[i]`, an `int` from 0 to 255 |
| constants | C's: floating ones are floats (`1.5`, `1.`, `.5`, `2e3`, `1.5e-3f`, hex floats `0x1.8p1`, `0x1p-3f`), or, without the `f`, doubles in arithmetic that has a double (below), integer ones ints (`2`, `010` in octal, `0x1F` in hex); `true` and `false`; enumeration constants, ints; integer constant expressions (a case's value, an enumerator's, an array's size) are C's, folded as FMinus computes |
| attributes | C23's `[[...]]`: after a function's parameters, clang's (above); after a parameter's or a field's name, before an array's sizes, FMinus's `fminus::range(min, max)` (a parameter's, or a field's of numbers: what a host's control or display spans, and the values `json_read` fills a field with) and `fminus::default(value)` (a parameter's, or a field's of numbers or `bool`s: what `json_read` sets the field to when a document does not fill it, which does not initialise it; a route's `string` parameter's a string literal, `fminus::default("saw")`, and a `string` field none, as `json_read` leaves one out), `json::name("name")` (the JSON member a field is read as) and `json::skip` (a field `json_read` leaves alone), and any other, the host's, which FMinus passes on as written (FMinus's own too): `float level [[ui::unit("dB")]] [[fminus::range(-60, 0)]];`, `float taps [[ui::hidden]] [16];` (but a name without a namespace that C23 does not have, `[[range(0, 1)]]`, is clang's warning, and ignored); their names identifiers, keywords and modules' names among them, as C23 reads them; their arguments numbers and C's string literals (C's escapes; adjacent literals are one) |
| comments | `// ...` and `/* ... */` |
| directives | `#include "file"`, the file read where the line is, as in C, and once only (as if every file began with `#pragma once`), found where the host says: `fminus` looks beside the including file, then in `-I`'s directories; C's conditionals, `#if`, `#ifdef`, `#ifndef`, `#elif`, `#else` and `#endif` (and C23's `#elifdef` and `#elifndef`), whose skipped groups are never scanned, on macros that `#define NAME value` and `#undef` make, or the host (`fminus -D NAME[=value]`), read in their conditions only, and FMinus's own, `__FMINUS_JSON__`, `__FMINUS_XML__`, `__FMINUS_WEB__`, `__FMINUS_WEB_SERVER__`, `__FMINUS_FILES__` and `__FMINUS_FILES_WRITE__`, defined (to 1) when the build has the JSON parser, the XML parser, the web library, its server, the file interface or its writing (`FMINUS_JSON`, `FMINUS_XML`, `FMINUS_WEB`, `FMINUS_WEB_SERVER`, `FMINUS_FILES`, `FMINUS_FILES_WRITE`), which neither a program nor the host may define or undefine; `#pragma`, ignored |
| types | `float` (IEEE single precision), `double` (IEEE double precision: for accumulators and slow state), `int` (32 bits, signed: C's `/` and `%`, overflow undefined) and `bool` (C23's: 0 or 1, computed as an int; a number is `x != 0` as a bool); `void`, a function's result; `string`, FMinus's: a read-only view of bytes something else owns, a pointer and a length (C's `struct { const char* data; int size; }`), no NUL after it and no encoding checked, compared by content, read by the `string_*` builtins, empty (null, 0) until set, and a literal one for ever; `char`, an array's element and a pointer's pointee only (`char name[32];`, `const char* p`), a byte read as an `int` from 0 to 255, where a program keeps text (`string_copy`, `string_of`, `char name[8] = "abc";`); pointers to `float`, `double`, `int`, `float2`, `float4`, `float8`, `char` and modules, `const` (`const float*`, read-only through it) and `restrict` (C99's promise that nothing else reaches it), and `void*` (an address with no type, which converts to and from any pointer); the handles of the runtime's objects, `http_response` (a request the program made), `http_request` (one it received), `http_server`, `json_doc`, `xml_doc`, `file_content`, `file_listing` and `fft_plan`, each C's opaque handle (`typedef struct fminus_json_doc* json_doc;`): what a builtin gives and others take, a `void*` converted to one and back, as C converts it, and one of another kind refused; compared with `==` and `!=`, tested as a condition (null is false), and kept in arrays; no arithmetic, element or field, and no pointer to one; arrays of `float`, `double`, `int`, the float vectors, `char`, `string`, modules and handles, one or two dimensions deep; modules, by name (`OnePole`), from their declaration on; `enum Wave`, an `int`, after its enumeration; `float2`, `float4`, `float8` (OpenCL C's vectors, lane by lane, scalars broadcast); `poly float` and `poly bool`, as many lanes as the machine's vectors ([simd.md](simd.md)) |
| builtins | `printf` and `eprintf` (C's `printf`, to stdout and to stderr: a literal format, checked as clang checks `printf`'s, of `%d`, `%i`, `%f`, `%e`, `%g`, `%s` and `%%` with C's flags, width and precision; the bytes written; they allocate: [programs.md](programs.md), "Printing"); `fabsf sqrtf floorf fminf fmaxf` (LLVM intrinsics, exact) and `sinf cosf tanf expf logf tanhf` (by the math mode: the runtime's, with one result for a float wherever it is computed, by default), on floats and vectors (a double argument rounds to a float, as C's prototype converts it); `sqrt fabs floor fmin fmax fma` (C's double functions, exact, on doubles only; `fma` rounds once); `malloc calloc free` (C's, with `int` sizes); `reduce_add reduce_min reduce_max` (a vector to a float); `poly_lanes` (an int) and `poly_index`; `poly_load poly_store` (a poly float from or to a `float*`, with an optional mask); the FFT: `fft_create(n)` and `fft_create_complex(n)` (a plan, an `fft_plan`; they allocate), `fft_forward(plan, in, out)` and `fft_inverse(plan, in, out)` (real when one side is floats, complex when both are `float2`s; unnormalised), `fft_destroy(plan)`; `hash(n)` (an `int` mixed into a well-spread one) and `randf(n)` (a float uniform in [-1, 1) from it: white noise, `out[i] = randf(start + i)`); bytes to numbers, `bytes_floats(data, bytes, out, cap)` (raw little-endian float32s, as a program's own server sends them) and `bytes_pcm16(data, bytes, out, cap)` (little-endian 16-bit PCM, scaled to [-1, 1)), into floats: how many values the `bytes` bytes at `data` hold, the first `cap` of them written (a null `data` or a negative size is none), wait-free, so a nonblocking function may call them; JSON (`FMINUS_JSON`, below): `json_parse(data, bytes)` (a document, a `json_doc`, null for a text that is no JSON, over 64 MiB or nested deeper than 1024) and `json_free(doc)`, which allocate, and the reads, wait-free, at a path, a JSON Pointer literal with places for ints (`"/osc/%d/detune", k`): `json_float`, `json_double`, `json_int` and `json_bool` (the value, or their last argument when there is none of that kind), `json_count` (an array's or an object's size), `json_floats(doc, path, out, cap)` (an array of numbers, or of rows of them: how many, the first `cap` written), `json_choice(doc, path, "a", "b", ...)` (which literal a string equals, or -1), `json_base64_floats(doc, path, out, cap)` (a base64 string's float32s), `json_read(doc, path, ..., &module)` (the module filled from the object there, field by field under each field's name in one walk of it, and whether every field without a default was filled), `json_slice(array, count)` (an array's first `count` elements, or a pointer's, as a JSON literal's value and nowhere else) and `json_string(doc, path, ..., fallback)` (the string there, unescaped, a `string` until `json_free`, or the fallback); XML (`FMINUS_XML`, below), the same way: `xml_parse(data, bytes)` (an `xml_doc`, null for a text that is no XML document, over 64 MiB or nested deeper than 1024) and `xml_free(doc)`, which allocate, and the reads, wait-free, at a path, a small part of XPath with places for ints (`"/preset/osc[%d]/@detune", k`: child steps by name, an index **from 0**, a last `@attribute`), of an attribute's value or an element's text: `xml_float`, `xml_double`, `xml_int` and `xml_bool` (the value, or their fallback), `xml_count` (how many elements the last step matches), `xml_floats(doc, path, out, cap)` (a list of numbers, separated by white space or commas) `xml_choice(doc, path, "a", "b", ...)` and `xml_string(doc, path, ..., fallback)` (an attribute's value or an element's text, trimmed, a `string` until `xml_free`, or the fallback); the web (`FMINUS_WEB`, below; [web.md](web.md)): `http_get(url, ...)` and `http_post(url, data, bytes, content_type, ...)` (a request, an `http_response`, at once; the URL a literal whose `%d`s are the ints after every other argument, or whose holes name its values where they go, `"https://example.com/presets/{name}.json"`, an `int`, a `bool`, an enumeration's value or a `string`, percent-encoded; the content type a literal), `http_post(url, json ..., ...)` (its body a JSON literal, sent as `application/json`) and `http_cancel(r)` and `http_close(r)`, which allocate, `http_wait(r, timeout_ms)` (the status, once done or the time is up), which blocks (refused in a nonblocking function, allowed in a nonallocating one), and the reads, wait-free: `http_status(r)` (0 while pending, the HTTP status, or a failure, `HTTP_TRANSPORT`, `HTTP_TIMEOUT`, `HTTP_CANCELLED`, `HTTP_REFUSED`, `HTTP_LIMIT`, `HTTP_CERTIFICATE`, `HTTP_INVALID`, `HTTP_FAILED`, names a program uses without declaring them), `http_size(r)` and `http_body(r)` (a `const void*`, until `http_close`; of a request received too), `http_progress(r)`; a null handle does nothing and reads as a failed request; the web server (`FMINUS_WEB_SERVER`, below; [web.md](web.md), "The server"): `http_listen()` (the program's server, an `http_server`, where its host's policy says: nowhere by default) and `http_close(server)`, which allocate, `http_accept(server, timeout_ms)` (a request, an `http_request`, waiting for one), which blocks, and, wait-free, `http_port(server)` (its port, or `HTTP_REFUSED`, `HTTP_TRANSPORT`, `HTTP_CANCELLED`), `http_next(server)` (the oldest request waiting, or null), `http_is(r, "PUT /osc/%d/gain")` (whether the request is the route, a literal checked when compiled), `http_arg(r, i)` (a number its `%d` took), `http_query_float(r, "name", fallback)` and `http_query_int`, its text, each a `string` until the reply: `http_header(r, "name")` (its first field of the name, without the white space around it, or empty), `http_path(r)` (its path as sent) and `http_query_string(r, "name", fallback)` (a query's field as sent, nothing decoded), `http_size(r)` and `http_body(r)` (its body), and the replies, `http_reply(r, status)`, `http_reply_bytes(r, status, data, bytes, "type")`, `http_reply_text(r, status, "type", "format", ...)` (C's `printf` of `%d`, `%g`, `%.Ng`, `%s`, a `string`, and `%%`) and `http_reply_json(r, status, json ...)` (a JSON literal, written in the request's own buffer); the file interface (`FMINUS_FILES`, on by default, below; [files.md](files.md)): `file_read("folder/name.ext", ...)` (the file read whole into memory, a `file_content`; the name a literal, a folder the host maps and a path inside it, whose `%d`s are the ints after every other argument, or whose holes name its values, `"presets/{name}.json"`, a `string`'s joining its segment and checked when filled in, `FILE_INVALID` when it breaks a rule), `file_list("folder/*.ext", ...)` (a snapshot of a folder's files that fit, in their names' order, a `file_listing`), `file_read_nth(list, i)` (a `file_content`) and `file_close(h)` (of either), which allocate, and the reads, wait-free: `file_status(h)` (`FILE_OK`, or `FILE_MISSING`, `FILE_REFUSED`, `FILE_DENIED`, `FILE_LIMIT`, `FILE_FAILED`, `FILE_INVALID`, names a program uses without declaring them), `file_size(f)` and `file_data(f)` (a `const void*`, until `file_close`), `file_count(list)`, `file_name(list, i)` (the i-th file's own name, a `string` until `file_close`, empty outside the listing); with `FMINUS_FILES_WRITE`, `file_write("folder/name.ext", data, bytes, ...)` (the whole file, replaced in one step or not at all) and `file_append(...)`, which allocate, each giving a status, and each taking a JSON literal in place of its bytes, `file_write("out/{name}.json", json (preset))` (`file_append` adding a newline after it: JSON Lines); a null handle reads as no file. Each handle a builtin takes may be a `void*`, as before FMinus had handles, and a JSON or XML read's document a `const void*` too; a string's, every one wait-free, in every build: `string_size(s)` (its length in bytes), `string_byte(s, i)` (byte `i` as 0 to 255, or -1 outside it), `string_slice(s, from, to)` (the bytes from `from` up to `to`, each end clamped to the string; empty when they cross), `string_find(s, what)` (where `what` first occurs, or -1; an empty `what` at 0), `string_starts(s, with)` and `string_ends(s, with)`, `string_float(s, fallback)` and `string_int(s, fallback)` (the whole string read as a number as a query's is: no `+`, finite, an `int` that fits; else the fallback), `string_data(s)` (its bytes' address, a `const void*`), `string_copy(out, cap, s)` (its first `cap` bytes at most into a `char` array, and its length) and `string_of(data, size)` (a view of `size` bytes at `data`, a `const char*` or a `void*`: `string_of(http_body(r), http_size(r))` reads a body as text; a null `data` or a negative size is the empty string) |

## Where FMinus differs from C

Where FMinus differs from C, it is on purpose:

- **Floating constants are floats first**: `float`, `double` and `int` are the arithmetic types (no `long`, `unsigned`
  or narrower integers), and in C an unsuffixed `1.5` is a `double`, so `x * 1.5` of a float `x` is computed in
  double; in FMinus the constant is a float, and so is the product, as FMinus has always computed it. Unless the
  arithmetic the constant is part of has a double: then it is C's double, and the arithmetic computes as C's does, every
  step of it. A constant's arithmetic is the expression around it as far as `+`, `-`, `*`, `/`, `%`, unary `+` and `-`,
  the arms of `?:` and parentheses reach, and it has a double when one of its operands is one, when it is compared with
  one, or when its value goes to one: a `double` it initialises or is assigned to, a parameter, a result or a cast. So
  `double t = x * 0.1;` is C's double product, `double third = 1.0 / 3.0;` C's third, `d *= 0.999` multiplies by C's
  0.999 and `d < 0.1` compares with C's 0.1, while `float y = x * 0.1;` is a float product, and a program without
  doubles means what it meant before they came, bit for bit. Where an arithmetic has no double, its float result may
  differ from C's in its last bits: write the `f` suffix (`1.5f`) and a program means the same in both. Every example
  here does. An integer constant is an `int`, as in C, so `1 / 2` is 0; one too large for an `int` is an error, where C
  would make it a `long` ([extending.md](extending.md), recipe 33).
- **Pointers point to `float`, `double`, `int`, `float2`, `float4`, `float8`, `char`, `void` or a module, one level deep**: no
  pointers to pointers or to arrays, no `bool*`, no null pointer constant (`0` is an int; an uninitialised pointer is
  null) and no casts between pointer types. Converting a pointer to one of another type (other than through a `void*`),
  or dropping its `const`, is an error, where C warns. A 2-D array's name stands for a pointer to its first row, as in
  C (`float (*)[16]`): indexed, dereferenced, moved and compared as C's, but no variable, parameter or field holds one,
  and `&g[i]` is an error.
- **Arrays are C's, one or two dimensions deep**: of `float`, `double`, `int`, the float vectors, `char`, strings or modules (no arrays
  of three dimensions, of pointers or of `bool`), each size an integer constant expression from 1 to 2^30, and 2^30
  elements in all (no variable length arrays, and no zero-length ones, GNU's), and zeroed like every variable. An
  initialiser list may be shorter than the array, never longer, and a brace around one element (a 1-D array's, or
  inside a row) is an error too (C only warns of both). A 2-D array is no parameter: C's would be a pointer to its
  first row, so a function takes a row (`float*`), or the module that holds the array. A `const` one whose list is
  constants (and of 1 MiB or less) is stored once, a constant of the program, as C stores a `static const` one: every
  call reads the same elements as C's would, but from one object, so its address is the same in every call, nested
  ones too, where C gives each call its own, and two equal tables may be one object
  ([extending.md](extending.md), recipes 26 and 34).
- **The handle types are keywords**, where C's would be a header's typedefs (`typedef struct fminus_json_doc*
  json_doc;`): no variable or function may be named `json_doc`, as none may be named `float4`. A handle is C's pointer
  to a struct the program never sees, and does what C lets one do, but for `<`, `<=`, `>` and `>=`, which C allows
  between two of a kind and FMinus refuses (their order means nothing), and arrays of them, which FMinus has though it
  has no other arrays of pointers. A `void*` still converts to any handle, as in C, so a program written with `void*`
  before the handles came compiles as it did, unchecked as it was; and a JSON or XML read still takes a `const void*`
  document, which C would refuse for a typed one (the read only reads it). A handle is no parameter declared as an
  array (that would be a pointer to one), and `call()` and `--run` treat one as they treat a `void*`
  ([extending.md](extending.md), recipe 44).
- **`defer` is ISO/IEC TS 25755's**, a Technical Specification beside C, not yet C itself: `_Defer`, and `defer`, which
  the TS encourages, both keywords (FMinus substitutes no macros, so `<stddefer.h>`'s way is not open). It is a
  rewrite when the program is compiled, as clang's `-fdefer-ts` makes it: each exit of a block holds the code of the
  deferred statements of the blocks it leaves, with no flag or list at run time. A call that the loop budget or a
  bounds check stops returns at once, as it always has, and runs no deferred statement on the way, as the TS says of
  `exit` and a signal: those checks are made in the optimised code, where the blocks are gone
  ([extending.md](extending.md), recipe 44).
- **`if` and `switch` take C2y's declaration in their head** (N3356, as N3580 refines it), which C23 does not have.
  FMinus has no storage classes, so N3580's constraint on them has nothing to check.
- **The top level holds no variables**, only constants: a declaration there is `const`, its initialiser constants, a
  constant of the program, as C's file-scope const object is (an LLVM constant global of its name, of 1 MiB at
  most), which no call fills or copies. A program's state lives in its modules, each instance's own. C has
  file-scope variables; what it takes for a constant initialiser FMinus takes too, and a constant scalar of the top
  level, which C17 leaves to the compiler (6.6p10) and clang accepts (`const int M = N * 2;`), but not a table's
  element (`table[1]`), which clang also accepts, nor what C leaves undefined, which clang refuses for `1 / 0` but
  folds to a value for `(int)1e10f` and `INT_MIN / -1`. The global is the program's own, as a `static const` one is
  a C file's: no symbol of its name, for a call by that name or a host to find. A constant is no integer constant
  expression, in C17 as in FMinus: an array's size, a case's value or an enumerator's is named by an enumeration
  constant, C's idiom (`enum { ROWS = 17, COLS = 12 };`). A constant scalar reads as its value, as clang reads one,
  with no load ([extending.md](extending.md), recipe 28).
- **Modules are FMinus's own**, C++'s structs with member functions, and less: no constructors (a module's fields
  start at 0 wherever it lives, and take no initialisers: a method such as `init` sets them), no const methods, no
  inheritance, overloading, access control or static members. A module goes to a function and back by pointer only
  (C++ would copy it), and `?:` does not choose between two of them. Its name is a type from its declaration on, as a
  C struct's is, so a module holds only modules declared before it (and pointers to any). A method is named as C++
  qualifies it, `OnePole::tick`, in errors and to a host.
- **Parameters are FMinus's own**, as C has nothing like them: `param` fields of a module, which the host sets and its
  methods only read, as `cutoff[i]`. How one reads is the host's choice at compile time: a knob (its value, loaded
  atomically once when the method starts), a stream (a `float` a sample, from a buffer the host points it at, which
  nothing the method writes may overlap, as with `restrict`, or, when the pointer is null, the slot's value, for the
  whole call) or frozen (a constant). `fminus::range` and `fminus::default` are for the host; FMinus checks them against
  each other only, and passes on any other attribute, as a field's (below). From `-O1`, a method whose loop branches on
  a `bool` knob or an `int` knob whose range holds at most 8 values, or whose loop reads a stream, gets a copy of its
  code for each value (for a stream, null or not), which computes what the method does
  ([extending.md](extending.md), recipe 16). A stream that gets no copies (a method's fourth stream, under the
  default budget of 8 copies) still reads with no branch: its loop vectorises when the stream has samples, and runs a
  sample at a time when it is flat.
- **A field's attributes are the host's**: C23 ignores an attribute it does not know, and clang warns; FMinus passes a
  field's or a parameter's on to the host as written (`Module::Field::attributes`), checking only that its numbers
  are numbers, for the host to decide what `[[ui::unit("dB")]]` means. Its own namespaces, `fminus::` and `json::`, are
  checked (an unknown name is an error), and so is clang's, whose attributes are a function's; a function's unknown
  attribute, a local's and a constant's, and one after an array's size (C23's, for the array's type) are errors too.
  A name without a namespace is C's: one of C23's standard attributes goes to the host, and any other,
  `[[range(0, 1)]]`, is clang's warning (`unknown attribute 'range' ignored [-Wunknown-attributes]`, with a note,
  "did you mean 'fminus::range'?"), and goes nowhere. A string literal is C's, and an attribute's argument or a
  builtin's (below) ([extending.md](extending.md), recipes 35 and 50).
- **`string` is FMinus's own, and `char` only an element**: text is a read-only view, a pointer and a length over
  bytes something else owns (C would need `typedef struct { const char* data; int size; } string;` and a function for
  `==`), where C's is a `char*` that a NUL ends. A piece of a request's path or of a JSON document has no NUL after it,
  so a C string would be a copy, a length a scan, and every overrun undefined; a view reads what the runtime already
  holds, allocates nothing, and every operation on it is wait-free, allowed in a `[[clang::nonblocking]]` function.
  Nothing joins or builds one: text is made only where it goes (a hole of an address, a JSON literal, a reply's
  `%s`). `==` and `!=` compare two by
  content; `<`, `+`, `[]`, a cast and a test as a condition are errors (`string_size(s) > 0` asks whether one is
  empty). **A string lives as long as what it points into**, the rule `http_body`'s pointer has: one over a `char`
  array as long as the array, a literal for ever, one that came from a request until the request's reply, one from a
  document until `json_free` or `xml_free`, and one from a listing until its `file_close`. The compiler does not check
  it. A string is no `param`, no vector's lane, and no
  parameter or result of a function the program declares and does not define (C's ABI passes a struct by each
  platform's own rules); `main` may take one, an argument's own text, which lives as long as the process, `--run`
  passes one, its argument's text, and `call()` passes none. Text comes from a literal, an argument of `main`, a request (a route's
  `string` parameter, `http_header`, `http_path`, `http_query_string`), a document (`json_string`, `xml_string`) or a
  listing (`file_name`), each a view of the bytes it already holds ([extending.md](extending.md), recipe
  48). `char` is a byte an array holds or a pointer points to, never a variable, a
  parameter or a result, and an element reads as an `int` from 0 to 255 on every machine (C
  leaves a plain `char`'s sign to the implementation; FMinus's is unsigned, as clang's `-funsigned-char` makes it); a
  store keeps the low 8 bits. A `char` array is read and written as `char`s only: FMinus's `char` has none of C's
  licence to alias other types. `string`, `char` and `when` are keywords ([extending.md](extending.md),
  recipe 46).
- **`when` is FMinus's own**, as C has no state but what a program declares: `when (c) s` is `if (c && !was) s;
  was = c;`, `c` computed once and `was` stored before `s` runs (so a `return`, `break` or `continue` in `s` does not
  arm it again), `was` a `bool` the compiler keeps in the instance of the method's module, after the declared fields,
  one for each `when`. So it is a method's statement (anywhere else an error that says where its state would live);
  `sizeof` a module grows by its `when`s' bools, which `module()` lists no field for but its size and alignment count,
  and a copy of the module copies them; it is refused in a `parallel for`'s iteration and a `parallel { }` block's
  statement, where it would change the module from several threads, as a method that changes its module is; a
  `poly bool` condition is an error (a `when` is one decision). It allocates and waits for nothing, so a
  `[[clang::nonblocking]]` method may hold one ([extending.md](extending.md), recipe 47).
- **A string literal is a `string` where one is wanted, and otherwise an argument, never a value.** It is a value of
  type `string` as an initialiser, an assignment's value, an argument or a result of the program's own function, an
  operand of `==` or `!=`, an arm of a `?:` that gives a string, or a `case` of a switch on a string; its bytes are a
  constant of the program, C's escapes resolved and adjacent literals one, with no NUL counted (`"a\0b"` is three
  bytes). It is an attribute's argument, and the argument of a builtin's string parameter, which gets its bytes and
  their length and takes only a literal (a `string` variable there is an error). Anywhere else, where C would have a
  `char` array, it is an error, in clang's words and at clang's column where C converts one with a warning or refuses
  it (`passing 'char[2]' to parameter of incompatible type 'float'`), and in FMinus's, at the literal, where C would
  take it silently (as a `void*`, a `bool`, an operand). `char name[8] = "abc";` is C's: the literal's bytes, the rest
  0, an error if they do not fit. A builtin's format, a literal with places for ints, is checked as clang checks
  `printf`'s, but takes only `%d` (an `int`, an enumeration's or a `bool`) and `%%`, and its mistakes, clang's
  warnings, are errors ([extending.md](extending.md), recipe 38). A path into a JSON document is such a
  format and a JSON Pointer too, and one into an XML document such a format and an XML path, each prepared when the
  program is compiled; a malformed one is an error at its column.
- **An address names its values in holes, `{name}`**, where `printf` has `%d` and the values after the format:
  `http_get("https://example.com/presets/{name}/{k}.json")`, `file_write("presets/{name}.json", data, bytes)`,
  `route "PUT /presets/{name}" (string name)`. A hole is a variable in scope (Rust's rule for its formats: a name,
  not an expression), an `int`, a `bool`, an enumeration's value (written in decimal, as `%d` is) or a `string`, and
  a literal has `%d`s or holes, never both; `{{` and `}}` are a brace. What a string may become is the library's to
  check where it goes: in a URL, percent-encoded, every byte but the unreserved ones, so it cannot add a `/`, a `?` or
  a `#`, and never in the scheme or the host (a port takes an `int`); in a file's name, one segment's part, never the
  folder or the extension, checked by the rules a literal's segments are (`FILE_INVALID` otherwise); in a route, a
  whole segment. The host's lists keep a `*` for each hole, as for each `%d` ([extending.md](extending.md),
  recipe 48).
- **JSON is FMinus's own**, as C has none: builtins on yyjson ([extending.md](extending.md), recipe 39).
  `json_parse` reads RFC 8259's JSON and nothing else (no comments, trailing commas, `NaN` or byte order mark; its
  strings' UTF-8 checked), and gives no document for more than 64 MiB of text, or for arrays and objects nested deeper
  than 1024 (`FMINUS_JSON_MAX_BYTES` and `FMINUS_JSON_MAX_DEPTH` in `fminus/Config.h`). A read never fails: a null
  document is an empty one, and a value that is absent or not of the read's kind gives its fallback. A number read as
  a `float` is the nearest float to its digits, read straight from them (never a `double` narrowed, which is now and
  then one bit off) and in no locale; `json_int` gives the fallback unless the number is an integer an `int` holds
  (`7.0` is 7, but `1.5` is not truncated and `3e9` not saturated). Parsing and freeing allocate, and are refused in a
  `[[clang::nonallocating]]` or `[[clang::nonblocking]]` function; every read is wait-free in the runtime, and allowed
  in both. Wait-free is not cheap: a read's time is bounded by the document, not by a constant. A step looks through
  an object's members, or an array of arrays or objects, up to the one it names (an index into an array of numbers
  and strings goes straight there), and a value is read whole, every digit of a number and every character of a
  base64 string, so one read of a large hostile document can take milliseconds, and a `%d` loop over n members or
  elements takes time in n². Read a document in `init`, into arrays, and keep the audio thread to those. A string comes
  out of a document as a view, `json_string` (unescaped as the parser holds it, until `json_free`), or `json_choice`
  says which of the program's literals it equals. `json_read(doc, path, ..., &module)` fills a module from the object at
  a path in one walk of its members, by a table of the module's fields the compiler makes once a type
  ([extending.md](extending.md), recipe 50): each field from the member of its name, or of its
  `[[json::name("...")]]`, and only from a value of its kind inside its `fminus::range` (an `int` from an integer an
  `int` holds, an enumeration from its constant's name as written, `"SAW"`, or its number, a `char` array from a string
  that fits with room for a 0, an array from as many elements as fit); a member no field names is ignored. A field not
  filled is set to its `fminus::default`, or left as it was, and the result says whether every field without a default
  was filled. **A field's `fminus::default` does not initialise it**: a module's fields start at 0, as C's do, and the
  default is what `json_read` sets when a document does not fill the field. **A `string` field is not filled** (it would
  point into the document, which the module outlives), nor a `param` (its value is the host's), a pointer, a handle, a
  poly type or a field with `[[json::skip]]`; an array of modules the document does not have is left as it was, its
  elements' defaults unset. It is wait-free, as the other reads are, and costs a walk of the object, each member
  compared with the fields not yet named.
- **XML is FMinus's own too**, on pugixml, made as the JSON builtins are ([extending.md](extending.md),
  recipe 40): the same fallbacks, effects, limits (`FMINUS_XML_MAX_BYTES`, `FMINUS_XML_MAX_DEPTH`) and null
  document. **An index in an XML path counts from 0**, as C's arrays and JSON Pointer's do, where XPath's counts from
  1: `"/pack/table[0]"` is the first table, which XPath writes `/pack/table[1]`. The path is a small part of XPath:
  child steps by element name from the root, `[n]` after a name, and a last `@attribute`, with no axes, predicates,
  wildcards or functions; a name matches as written, its prefix too (`"/preset/fm:depth"`), since there are no
  namespaces. `xml_parse` is a reader, not a validator: it refuses what is not well formed, but takes invalid UTF-8,
  for one, though not a character reference to U+0000 (`&#0;`), which would cut a value short. It skips a DOCTYPE,
  so no entity a document declares is ever expanded and none outside it fetched (a "billion laughs" document is
  inert); it expands XML's predefined and character references. A value is an
  attribute's, or an element's first text (CDATA too; text either side of a comment is one), without the white space
  around it; numbers are XML Schema's (`+1.5`, `007`, `INF`, `NaN`), read as JSON's are; a bool is `true`, `false`,
  `1` or `0`. A read costs what a JSON read does: an element is found among its parent's children, an attribute among
  its element's, and a value is read whole.
- **The web is FMinus's own**, as C has none: builtins on libcurl, in a build with `FMINUS_WEB` ([web.md](web.md),
  and [extending.md](extending.md), recipe 41). A URL is a string literal, a format whose ints come after
  every other argument, as `printf`'s do, or holes (above), and is checked where it is written: printable ASCII with
  no space or
  backslash (no CR or LF), `http` or `https`, no credentials. Its requests reach only what its host allows
  (`Program::Options::web`: none by default), and the host can list every URL before anything runs, a hole a `*`
  (`Program::webRequests()`); the library checks each request, each redirect and each connection's address. A request
  is an `http_response` that comes back at once; failures are statuses (`HTTP_REFUSED`, `HTTP_TIMEOUT`, ...), never traps.
  `http_wait` is a builtin that blocks: refused in a `[[clang::nonblocking]]` function, allowed in a
  `[[clang::nonallocating]]` one. A program that uses a table on the block it arrived in gives up determinism: fetch in
  `init`, then play.
- **So is its server**, in a build with `FMINUS_WEB_SERVER` too ([web.md](web.md), "The server", and
  [extending.md](extending.md), recipe 42). C has no callbacks a program could be handed a request through,
  nor do FMinus programs, so a program **takes** its requests (`http_next`, wait-free, on the audio thread between
  blocks; `http_accept`, which blocks, on a thread of its host's) and answers each with a reply builtin. A route is a
  string literal, a method and a path whose `%d`s are numbers of the request's path, checked where it is written and
  listed for the host (`Program::webRoutes()`); a reply's text is a format of C's `printf`, `%d`, `%g`, `%.Ng`, `%s` (a
  `string`) and `%%`, checked as clang checks `printf`'s and written the same in every locale. Where it listens is the
  host's
  (`Program::Options::web.server`: nowhere by default), never the program's: `http_listen()` takes no argument. The
  library answers 404 and 405 for what no route names, and checks `Host`, `Origin`, `Sec-Fetch-Site`, the token and
  every limit before a request reaches the program. A program reads a request's text only as views of what the library
  holds (`http_path`, `http_header`, `http_query_string`), until its reply; with a token, `Authorization` reads empty.
- **`serve`, `route` and `reply` are FMinus's own statements** over those builtins, as `parallel` is ([web.md](web.md),
  "serve, route and reply", and [extending.md](extending.md), recipe 45): a `serve` takes the
  requests waiting for its own routes only (wait-free; with a timeout it blocks), a route's parameters are typed and
  filled from its literal's holes, `{name}` (an `int`, or a `string`, a whole segment), and the query (a `string` its
  bytes as sent, nothing decoded; what does not fit is 404 or 400 before its block runs), and
  `reply` answers the route's request and leaves the route, which every path through the route must reach, so a request
  a serve takes is answered once. Two routes one request could match are an error unless one is more specific (Go's
  `ServeMux` rule: a literal segment before a number's hole before a string's). `serve`, `route`, `reply` and `json` are
  keywords: nothing may be named so.
- **`within` is FMinus's own statement** too ([web.md](web.md), "within", and
  [extending.md](extending.md), recipe 49): `within (ms) { ... }` is `parallel { }`'s promise for requests,
  Trio's `move_on_after` with C's syntax. Where its block ends, by its brace, `return`, `break`, `continue` or a
  `reply`, every request the thread running it started in it has ended: the end waits for them until the deadline and
  cancels what is left (`HTTP_TIMEOUT`); and the deadline bounds every wait in it (`http_wait`, `http_accept`, a
  `serve` with a timeout). An inner block cannot outlast an outer one. It waits, and its beginning allocates, so a
  `[[clang::nonblocking]]` function refuses it in the words it refuses `http_wait` in, and a
  `[[clang::nonallocating]]` one as an allocation. A call the loop budget or a bounds check stops leaves no block of
  its thread's behind (its requests go on as any other), though it runs no deferred statement. `within` is a keyword;
  `await` is not.
- **A JSON literal is an argument, never a value**, as a format is: `json { "gain": gain, "name": name, "taps": taps
  }`, `json [a, b]` or `json (preset)` (one value), the body of a `reply` and `http_reply_json`, of `http_post` (sent as
  `application/json`) and of `file_write` and `file_append` (which adds a newline: JSON Lines), which the runtime's JSON
  writer writes straight where it goes (a reply's own buffer; a POST's or a file's buffer of exactly the text's size,
  measured first) from a description the compiler makes: a float's and a double's numbers the shortest digits that read
  back the same, a NaN or an infinity `null`, a `string` (or a literal) a JSON string escaped as RFC 8259 asks (`"`, `\`
  and the controls; bytes above 0x7F as they are), **an enumeration's value its constant's name** (`"SAW"`; a value no
  constant has, its number), a `char` array its text, arrays (of two dimensions too) and vectors JSON arrays, a module an
  object of its fields under their JSON names (as `json_read` reads them; a `string` field too; a parameter only as
  `*this` in its own method), and `json_slice(array, count)` an array's first elements ([web.md](web.md), "The
  JSON literal"). It needs the JSON writer, part of `FMINUS_JSON`. FMinus has no JSON value, and C no literal of the
  kind.
- **Files are FMinus's own too**, in place of C's `<stdio.h>`, in a build with `FMINUS_FILES` (on by default;
  [files.md](files.md), and [extending.md](extending.md), recipe 43). A program never names a path
  of the machine: a name is a string literal, a folder its host maps and a path inside it (`"presets/%d.json"`, its
  ints after every other argument, or `"presets/{name}.json"`, a hole), checked where it is written (no `..`, no
  hidden file, no device name of Windows,
  nothing Windows would read otherwise, an extension the literal writes) and listed for the host before anything runs
  (`Program::files()`). It reaches only the folders its host grants (`Program::Options::files`: none by default), and
  the runtime decides from what it opened, not from the name's text: each directory opened beneath the last, no link
  followed, each compared by identity with the denied ones, the file a regular one within the size limit. A file is
  read whole into memory, never mapped, and every builtin that touches the disk is refused in a
  `[[clang::nonblocking]]` function; reading a handle is wait-free. Failures are statuses (`FILE_REFUSED`,
  `FILE_MISSING`, ...), never traps. Writing (`FMINUS_FILES_WRITE`) and absolute names (`FMINUS_FILES_ANYWHERE`) are off
  by default, and a build can limit the types and the size of any file it touches, which no host can raise.
- **The heap's memory is aligned to 64 bytes and zeroed**, `malloc`'s too, as everything FMinus declares starts at 0;
  sizes are `int`s (C's are `size_t`s), so one block holds at most 2 GB, and `sizeof` of a larger type (a module of
  big arrays) is an error. A call that breaks a function's `[[clang::nonallocating]]` or `[[clang::nonblocking]]` is
  an error, where clang only warns (`-Wfunction-effects`). As with clang's `malloc`, LLVM may remove a block the
  program never uses, and a removed block does not fail: a test of it against null is then false.
- **The FFT is FMinus's own**, as C has none: builtins on PFFFT, which take sizes of 2, 3 and 5 only, multiples of 32
  points for a real plan and of 16 for a complex one (`fft_create` is null for any other). A plan is made and freed off
  the audio thread (the effects count `fft_create`, `fft_create_complex` and `fft_destroy` as allocations), and a
  transform allocates nothing. Bins are `float2`s, (re, im), and neither direction scales: inverse(forward(x)) is n
  times x, as in FFTW and PFFFT. A plan is opaque: a program hands it to the FFT's builtins, and reads or writes
  nothing through it.
- **`hash` and `randf` are FMinus's own**, as C has nothing like them (its `rand` keeps a state, and gives the C
  library's numbers): `hash(n)` mixes an `int` into a well-spread one (Chris Wellons's lowbias32 on its 32 bits, a
  bijection), and `randf(n)` is the hash's top 24 bits as a float uniform in [-1, 1) (k / 2^23, k from -2^23 to
  2^23 - 1). Each is a few integer instructions inline, with no state: the same `n` gives the same bits wherever it is
  computed, in every lane of a vectorised loop, at every level, in every math mode and on every machine, so white noise
  is `out[i] = randf(start + i)`, about three instructions a sample on arm64. A program declares no function of
  either name ([extending.md](extending.md), recipe 25).
- **Memory is read and written as the type it holds**, C's strict-aliasing rule, which FMinus tells LLVM as clang
  does (TBAA): an `int` read as a `float` through a `void*` is undefined. FMinus is stricter in three places: a float
  vector or `poly float` is its elements' type, `float`; allocated memory keeps one type at each place until it is
  freed (C lets a store give it a new one); and FMinus's `char` is a type of its own, which aliases nothing else, so
  only a module's copy and a declaration's zeroing read or write bytes of any type (and `string_byte`, which reads what
  a string views, whatever it is). `--no-strict-aliasing` or `Program::Options::strictAliasing = false` turns
  the rule off, as clang's `-fno-strict-aliasing` does.
- **`sinf`, `cosf`, `tanf`, `expf`, `logf` and `tanhf` give a float one result wherever it is computed**: in a scalar
  loop or its vectorised body, in a vector of any width, at any optimisation level, from a constant or not, on any
  machine. C promises no such thing. By default they are FMinus's own kernels, one sequence of IEEE operations each,
  within 1.56 ulp of the exact result (tan 3.35, exp 1.05, log 0.99; libm's are within 1), inline from `-O1`;
  `fminus::math` computes the same in C++ ([extending.md](extending.md), recipes 23 and 24).
  `tanf` became a builtin with the kernels: a prototype of it, `float tanf(float x);`, which called libm's, now calls
  the builtin, as a prototype of any builtin does, and a program's own function named `tanf` is an error ("'tanf' is a
  builtin and cannot be redefined"): rename it. `Program::Options::math` (`fminus --math`) chooses `Libm`, libm's
  results bit for bit, or `Fast`, LLVM's intrinsics as clang compiles `<math.h>`. The runtime keeps three names for
  each of them (`fminus_rt_sinf`, `fminus_rt_sin_f4` and `fminus_rt_libm_sinf` for `sinf`): a program that calls one
  may not declare a function of any of its names, in any math mode. The call is then an error, with a note at the
  declaration.
- **A double's math is C's double functions that are instructions**: `sqrt`, `fabs`, `floor`, `fmin`, `fmax` and
  `fma`, exact (one rounding each, `fma`'s too), the same in every math mode, and on doubles only (a vector argument is
  an error). FMinus has no double `sin`, `cos`, `tan`, `exp`, `log` or `tanh`: its kernels are a float's, and `sinf(d)`
  rounds the double to a float first, as C's prototype converts it. A program that wants the C library's declares it,
  `double sin(double x);`, as any external function, off the audio thread's guarantees (the error for a call without
  the prototype says so). `sqrt` and the other five became builtins with the type, as `tanf` did: a program's own
  function of one of their names is an error, and a prototype of one that is not C's (`float sqrt(float x);`)
  conflicts.
- **Subnormals are C's unless the host asks for flush-to-zero** (`Program::Options::denormals`,
  `fminus --denormals=flush`): then a subnormal input reads as 0 and a subnormal result is 0, each with its sign, a
  float's and a double's, in folding as at run time (but for a float/double conversion of a subnormal that only
  LLVM's optimiser finds constant, which it folds as IEEE's: docs/extending.md, recipe 33), and each entry point (a
  `[[clang::nonblocking]]` function the program does not call) sets the mode on entry and restores the caller's on
  return. C's floats underflow gradually; clang's `-fdenormal-fp-math=preserve-sign` is the same choice.
- **Vectors are OpenCL C's**, which C has none of: `float2`, `float4`, `float8`, their literals and swizzles, and math
  functions that take them. A comparison of vectors gives a lane mask (OpenCL C gives 0 or -1 in an `int` vector), and
  `?:` with a vector condition picks each lane, so both sides run. Literals come in OpenCL C's form,
  `(float4)(a, b, c, d)`, and in C's, `(float4){a, b, c, d}` (with clang's `ext_vector_type`). They differ: C reads
  `(float4)(...)` as a cast of a comma expression. `poly float`, a vector as wide as the machine's, is FMinus's own
  ([simd.md](simd.md)). A double is no lane: a double meeting a vector is clang's error for the truncation
  (`v * d`, `float4 v = d;`), there is no `double4` or `poly double`, and a vector literal's double rounds to its lane's
  float, as the literal's type says (`(float4)(d)`).
- **Threads are FMinus's own, and structured**, as OpenMP's `parallel for` and `sections` are, where C has a library
  (C11's `<threads.h>`: threads a program makes, joins and locks around). `parallel for (int i = first; i < end; ++i) s`
  runs its iterations at once, spread over the machine's cores, and `parallel { a = f(x); b = g(y); }` its statements,
  each a task; either has finished, every iteration or statement, where it ends. A loop counts up by one from an `int`
  it declares (`i < end` or `i <= end`; `++i`, `i++` or `i += 1`), and its bounds are computed once, before it starts,
  where C tests a `for`'s condition before each iteration. The compiler refuses what it can see would race: an iteration
  that assigns a variable from outside the loop (every iteration would), assigns its own loop variable, or calls a
  method that changes its module on one; two statements of a block of which one changes a variable that the other uses;
  `break` and `return` out of either, and a declaration as a block's statement. What an iteration reaches through an
  index or a pointer (`out[i]`, `voices[v].render(n)`) is its own to keep apart from the others', as with OpenMP: the
  compiler sees which variable, not which element. There are no reductions: an iteration writes its own element, and the
  code after the loop combines them. A program without races computes what it would on one thread, bit for bit. A
  program's regions run on one pool for the process, a thread for each core but one, which the first `Program` with a
  region starts when it compiles (the calling thread works too); a region allocates nothing and takes no lock, so a
  `[[clang::nonblocking]]` `process` may have one, and every thread runs it in its caller's floating-point mode.
  `parallel` is a keyword: nothing can be named `parallel`; and a program with a region may not declare a function named
  `fminus_rt_parallel_for`, the runtime's ([extending.md](extending.md), recipe 22).
- **`%` works on floats**, with `fmodf`'s semantics: the result has the sign of the dividend, so `-7.5 % 2` is
  `-1.5`. C allows `%` on integers only; HLSL allows it on floats. `%=` goes with it.
- **A shift is defined for every count and value**, as OpenCL C defines it: `x << n` and `x >> n` shift by `n`'s low
  five bits (`n & 31`: `x << 32` is `x`, `x << -1` is `x << 31`), `<<` drops the bits shifted out of two's complement
  (`-1 << 1` is -2, `1 << 31` is the least `int`), and `>>` shifts the sign in (`-8 >> 1` is -4). C leaves a count
  below 0 or from 32 on undefined, and a left shift of a negative value or into the sign; a negative value's right
  shift is implementation-defined (clang's is arithmetic, as FMinus's). So a program means the same on every machine,
  which LLVM could not promise of a shift it may take for poison, and a scalar shift costs nothing for it (the shift
  instructions of arm64 and x86-64 take the count's low five bits themselves). `&`, `|`, `^` and `~` are C's; `+`,
  `-` and `*` on `int` still overflow as C's do, undefined. With no `unsigned`, a logical shift right is an arithmetic
  one and a mask: `(x >> 17) & 0x7FFF` ([extending.md](extending.md), recipe 25).
- **A function may be called before its definition** without a prototype, because the compiler declares every
  function first. A prototype with no definition declares an external C function, as in C: the JIT resolves it in
  the process (`float atanf(float x);` calls libm's), and the linker resolves it in an executable. On Apple systems,
  whose libm sets no `errno`, a prototype of libm's function says it writes no memory, as clang's `<math.h>` says
  there, so a call on a loop's invariants is made once, before the loop ([extending.md](extending.md),
  recipe 19).
- **Enumerations are C's, a little smaller**: `enum Wave` is `int` whatever its constants are (or C23's fixed type,
  `int` or `bool`), where GCC and clang make it an `unsigned int` when none is negative, and its constants are `int`s
  in an `int`'s range, where C23 lets a constant past it give the enumeration a wider type. A case's value or an
  enumerator's that overflows an `int` is an error, where clang warns. Messages name the type `'int'`, where clang
  names it `'enum Wave'` ([extending.md](extending.md), recipes 27 and 32).
- **A declaration without an initialiser starts at 0**, and a pointer at null, and so does a variable whose
  declaration a switch's jump to a label passes (`case 1:` after `case 0: float t = x;`). C leaves the variable
  indeterminate.
  From `-O1`, the optimiser removes a local's zeroing where nothing reads the zeros: an array's that loops overwrite,
  and a module's that a copy of another overwrites. `-O0` keeps every one ([extending.md](extending.md),
  recipe 18).
- **A non-void function that can reach its end is an error**, as in C++. Clang only warns in C. As in clang's check,
  a constant condition takes one branch: the end after a `while (1)` or `for (;;)` that only a `break` would leave, or
  after `if (1) return x;`, is not reached, and needs no return. A constant of the top level folds there too
  (`while (RUN)` with `const int RUN = 1;`), and a parameter never does, whatever the host binds it to, so a program
  compiles the same with a knob, a stream or a frozen value. clang also folds a block's `const` variable, `&&`, `||`
  and `?:` there; FMinus does not (`while (1 && 1)` asks for a return).
- **The preprocessor is `#include "file"`, the conditionals and macros that only they read.** A macro, the program's
  (`#define LEVEL 2`) or the host's, is read where C reads it as a condition, in `#if`'s and `#elif`'s expressions and
  by `#ifdef`, `#ifndef` and `defined`: its name in code is an error, never a substitution (for a constant, an `enum`
  or a `const`), so a program FMinus compiles means the same in C, and nothing is rewritten before the parser reads
  it. A function-like macro, a macro named as a keyword, `#error`, `#line` and `#include <...>` (FMinus has no system
  headers) are errors, and `#pragma` is ignored, as C ignores the pragmas it doesn't know. An `#if`'s arithmetic is
  64-bit and defined for every operand, as FMinus's code is (a shift takes its count's low six bits). Each file is
  read once, as if it began with `#pragma once`: two files that both include a third (a module both use) read it the
  first time only, where C would define everything in it twice. A directive is one line, which its `#` begins (a
  comment before the `#` is an error, where C reads the directive) and on which a `/*` comment in it ends, in a
  skipped group too. A directive's errors are clang's, `extra tokens at end of #endif directive` and a macro
  redefined with another value too, which clang only warns of ([extending.md](extending.md), recipes 21 and
  31).
- **Arithmetic is IEEE single precision**, with no contraction into fused multiply-adds (clang's
  `-ffp-contract=off`), and subnormals as IEEE has them unless the host flushes them (above).

- **`main` takes typed parameters, `int`, `float`, `double` and `string`**, where C's takes `argc` and `argv` (a
  `char**`, which FMinus has no type for). An FMinus executable reads one argument a parameter from its command line,
  each as its type says, a string as the argument's own text. An `int main` returns the exit status, as C's does; a
  `float`, `double` or `bool` `main` has its result printed, `true` or `false` for a bool ([programs.md](programs.md)).
- **`printf` is C's, in part, and `eprintf` FMinus's**: a literal format of `%d`, `%i`, `%f`, `%e`, `%g`, `%s` and
  `%%` with C's flags `-`, `+`, ` ` and `0`, a width and a precision, whose mistakes are errors (clang's `-Wformat`
  warnings); no length modifier, `*`, `#`, `%x`, `%c` or `%p`, and a width of at most 999 and a precision of at most
  99. Its digits are FMinus's own, in no locale, the same on every machine. `eprintf` writes to stderr, as C's
  `fprintf(stderr, ...)` would ([programs.md](programs.md), "Printing").

Loops make progress as C11 says (6.8.5p6), not as C++ does: a `while` or `for` whose condition is not a constant
expression and which runs for ever without effects is undefined, so LLVM may delete such a loop whose results go
unused, while `while (1)` and `for (;;)` may run for ever. A function as a whole is not assumed to make progress, as a
C++ one is. A host that would rather a loop did not run for ever compiles with a loop budget
(`Program::Options::loopBudget`, `fminus --loop-budget n`): no change to the language, a limit on what a call takes.

An index outside its array is undefined, as in C. A host that would rather it stopped the call compiles with bounds
checks (`Program::Options::boundsCheck`, `fminus --bounds-check`), for a development build: every access whose array
the compiler sees (`a[i]`, `m.taps[i]`, `voices[v]`, `a + i`) is checked against the array's extent, and a call whose
check fails returns early, with where it failed recorded for the host. No change to the language either: a program
within its arrays computes the same with the checks and without.

## Programs that are also C

A v0 program with `f` suffixes (on the floating constants of each arithmetic that has no double: one that has one
means C's double without it), with prototypes for calls ahead of a definition, with vectors only in C's form (braces,
given clang's `ext_vector_type`), and without `%` on floats, `poly`, modules, `parallel`, `main`, FMinus's own builtins
(`hash`, `randf`, the FFT), a shift that C leaves undefined or a negative value in a variable of an enumeration with no
negative constant (which GCC and clang make unsigned), is therefore also a valid C program with the same meaning, given
`<math.h>` (`<stdlib.h>` for the heap; C23 for the attributes and a declaration after a label) and `-ffp-contract=off`.
The example above and `examples/delay/delay.fm` compile as C under `-Wall -Wextra -Werror`, the latter with clang's
`-Wfunction-effects` too. One with modules is C++ instead, spelled `struct` for `module` and `__restrict` for
`restrict`, as long as no module has parameters, which are FMinus's own: `examples/voices/voices.fm`'s modules but
`Synth` compile so, with `-Wno-perf-constraint-implies-noexcept` (C++ wants a nonblocking function `noexcept` too). The
other examples use modules with parameters, `poly`, `parallel`, FMinus's own builtins (the convolver's FFT, the noise's
`randf`) or a `main`; hello's one function is C.

## Functions, errors and warnings

Every FMinus function becomes an external LLVM function of the same name with the C calling convention (a `float` is
LLVM's `float`, a `double` its `double`, an `int` an `i32`, a pointer a `ptr`), so C++ calls JIT'd code through a
pointer of the matching type: `float(float, ...)`, or `void(const float*, float*, int)` for a function that processes a
block of samples in one call. A method is one too, named `OnePole::tick`, whose first parameter is the module's address:
`float(void*, float)`. A module is an LLVM struct of its fields, laid out as C lays out a struct. The code generator
reports every error it finds, in source order and in clang's words, as `file:line:column: error: message`, and each
once: an operand with an error of its own (an undeclared name) is no operand for an operator, a subscript, `*`, `&`, a
call or a conversion to complain about, as clang's recovery has it, so `*(p + y)` is the one error about `y`. The parser
stops at the first syntax error. FMinus has two of clang's warnings. `-Wswitch`: a `switch` without a `default` on a
value of an enumerated type, whose cases miss some of its constants (`warning: enumeration value 'NOISE' not handled in
switch [-Wswitch]`). A value that folds to a constant (a constant of the top level) gets none, as in clang, which says
"no case matching constant switch condition" there instead; FMinus does not. And `-Wunknown-attributes`: a field's or a
parameter's attribute without a namespace that C23 does not have, which FMinus ignores (`warning: unknown attribute
'range' ignored [-Wunknown-attributes]`). The program still compiles; `fminus` prints the warnings with the errors, in
source order, as clang does.

## The driver, `fminus`

```bash
build-dev/bin/fminus chain.fm                      # the optimised IR (-O2)
build-dev/bin/fminus chain.fm -O0                  # the IR as the code generator wrote it
build-dev/bin/fminus chain.fm --emit ast           # the syntax tree, as s-expressions
build-dev/bin/fminus chain.fm --emit asm -o -      # this machine's assembly (--emit obj: an object file)
build-dev/bin/fminus chain.fm --run chain 1        # JIT-compiles, calls chain(1), prints 0.33333334
build-dev/bin/fminus clock.fm --run seconds 48000  # a function of doubles: --run passes and prints them
build-dev/bin/fminus mix.fm --run blend 0.5 3 true # bools, ints, floats and doubles, in any mix and number
build-dev/bin/fminus dsp.fm --run scale 1,-2,0.5 3 2   # an array for a pointer, printed after the call: x = {2, -4, 1}
build-dev/bin/fminus note.fm --emit exe -o note    # an executable that runs main: ./note 69 prints 440
build-dev/bin/fminus unison.fm --poly-lanes 16     # compile as for a machine with 16-float vectors (AVX-512)
build-dev/bin/fminus lp.fm --stream Lowpass::cutoff --freeze Lowpass::mode=2   # parameters: a stream, a constant
build-dev/bin/fminus lp.fm --knob-versions 0       # no copies of a method for its knobs' values (8 at most by default)
build-dev/bin/fminus synth.fm --math libm          # sinf, cosf, ... as libm's (vector, the default; fast: LLVM's own)
build-dev/bin/fminus lp.fm --denormals=flush       # subnormals flushed to zero: folded so, and set by entry points
build-dev/bin/fminus lp.fm --no-strict-aliasing    # no TBAA: for a program that reads memory as another type
build-dev/bin/fminus synth.fm -I lib               # #include "name": beside the including file, then in lib/
build-dev/bin/fminus synth.fm -D DEBUG -D LEVEL=2  # macros for #if and #ifdef, as clang's -D (-D DEBUG is 1)
build-dev/bin/fminus voices.fm --threads 4         # a parallel region on 4 threads at most (0: every core; 1: alone)
build-dev/bin/fminus osc.fm --run f 1 --loop-budget 1000000   # the loop budget: a call that would loop for ever
                                                   # returns early (status 3); stderr says what it took
build-dev/bin/fminus taps.fm --run tap 20 --bounds-check   # bounds checks: an index outside its array stops the
                                                   # call (status 4); stderr says where, the array and the index
build-dev/bin/fminus pedal.cir --emit fm           # the FMinus module a circuit netlist generates (docs/circuits.md)
build-dev/bin/fminus pedal.cir --self-test --emit fm   # the self-test chooses the netlist's open configuration first
                                                   # (--threads its threads), and prints a line of directives to paste
                                                   # and its warnings (a block that rings at Nyquist)
build-dev/bin/fminus pedal.cir --notes -o /dev/null    # the circuit compiler's plan: blocks, rates, solvers or linear
                                                   # forms (matched, Woodbury), the method check, latency
build-dev/bin/fminus pedal.cir --probes --nodes --emit fm   # the module with its probe fields, and its nodes as a
                                                   # probe sees them (rate, DC volts, readable or why not)
```

## How it fits together

How it fits together: `frontend::parse` (Lexer.l, then Parser.y) gives an `ast::Program`, and `ir::lower` (in
`src/ir/`, where CodeGen.cpp runs the parts that `Lowering.h` lists) gives an `llvm::Module`. From there, `jit::Jit`
optimises and compiles it in memory, or `support/Aot.h` writes it to a file.

Each variable is a stack slot (an `alloca` in the entry block), read with loads and written with stores, so
`fminus -O0` reads like `clang -O0`. From `-O1`, mem2reg promotes the slots back into registers.

**Adding a construct**, say comparisons and `?:`. [extending.md](extending.md) works through this one and
the others it builds on, all the way to C's `if`, `while` and `for`, and [simd.md](simd.md) through vector
types:

1. Its tokens: a rule in `src/frontend/Lexer.l` (keywords before `{IDENTIFIER}`), a `%token` in `Parser.y`.
2. Its grammar: a rule in `Parser.y`, with C's precedence if it is an operator. Bison's report is
   `build-dev/src/generated/frontend/Parser.output`, and `-Wcounterexamples` shows an input for each conflict.
3. Its node: an alternative of a `std::variant` in `src/frontend/Ast.h`, or an enumerator. Every `std::visit` and
   every `switch` over it now fails to compile until it handles the new node: `dump` in `Ast.cpp`, and the lowering
   in `src/ir/`, in the file its kind belongs to (statements in `Statements.cpp`, vectors in `Vectors.cpp`, ...;
   `Lowering.h` has the list).
4. Its tests: a dump in `tests/unit/FrontendTest.cpp`, a result in the code generator's tests (`CodeGenTest.cpp`,
   `ControlFlowTest.cpp`, `SwitchTest.cpp`, `EnumTest.cpp`, `IntTest.cpp`, `BitwiseTest.cpp`, `PointerTest.cpp`,
   `HeapTest.cpp`, `EffectTest.cpp`, `ArrayTest.cpp`, `TableTest.cpp`, `TopLevelTest.cpp`, `ModuleTest.cpp`,
   `FftTest.cpp`, `HashTest.cpp`, `VectorTest.cpp`, `PolyTest.cpp`, `ParallelTest.cpp`, `DoubleTest.cpp`,
   `Array2DTest.cpp`, `FieldAttributeTest.cpp`, `StringTest.cpp`, `BytesTest.cpp`, `JsonTest.cpp`, `XmlTest.cpp`,
   which share `Compile.h`), IR or errors in `tests/lit/fminus/*.fm`
   (`// RUN:` lines, checked by FileCheck).

## The examples

`examples/biquad/` is a kernel to grow the language around. It is a biquad low-pass module with coefficients recomputed
every sample, its cutoff a stream, which filters a block a call, with a harness that calls it once, compares it with C++
and times both, and a plan of features in its README. `examples/delay/` is a feedback delay whose line the program
allocates, with a kernel marked `[[clang::nonblocking]]`, written as plain C and as a module. `examples/saturator/`
(tanh) and `examples/meter/` (peak and RMS) each run a loop over samples against a loop over `poly float`s, and time
both. `examples/voices/` is a synth of eight voices in modules, its controls knobs, which a host of the public API
allocates and runs. `examples/convolver/` convolves with an impulse response by FFT. `examples/ensemble/` renders
sixteen voices at once with a `parallel for`, and checks that every core gives what one thread does, bit for bit.
`examples/parameters/` sweeps a filter's cutoff with an LFO, through a knob once a call or a stream a sample, and
freezes a fixed one. `examples/wavetable/` loads a wavetable pack from a JSON or an XML document its host hands it, off
the audio thread, and plays it on the audio thread.

`examples/plugin/` is FMinus inside a plugin: FMinus Filter, a JUCE effect (AU, VST3 and Standalone on macOS) whose
DSP, a state-variable filter, is an FMinus program that its processor compiles in `prepareToPlay`, with the cutoff a
stream from a smoother, resonance and mode knobs (the program crossfades a new mode itself), and the sample rate frozen
at the host's. The processor refuses a `process` that isn't `[[clang::nonblocking]]`, clears the filter on a host's
`reset` through an atomic flag, and each bundle exports only its entry points, LLVM's symbols kept inside. It is a
CMake project of its own (`cmake -S examples/plugin -B build-plugin`), on JUCE 8.0.4 (pinned by commit in
`cmake/FMinusPins.cmake`) and the LLVM that FMinus's own build installed in `.deps/`, with a headless check,
`fminus-plugin-check`, which its README says how to run.
