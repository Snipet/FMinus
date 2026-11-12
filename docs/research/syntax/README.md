# Syntax for networked programs: statements, text and JSON

*Research, 2026-11-12, written against FMinus at `47025c9`: the web library, its server and the file interface as
[web.md](../../web.md) and [files.md](../../files.md) describe them. The question asked: research better syntax for
the web features, since standard C syntax is not favourable for networking code, and since the networking and file
features are meant to take FMinus beyond DSP. Four answers were given before this was written: design for all four of
small network services, scripts and tools, networked audio and devices, and numeric and data work; stay at "C plus new
statements", as `module`, `param` and `parallel for` were; none of today's guarantees is sacred, and text may be part
of the language; and all four of handles and lifetimes, routing, data in and out, and waiting are what hurts. Nothing
under `src/`, `include/`, `tools/` or `cmake/` changed for this note. §12 says what was measured, what was read at its
source, and what was neither.*

## 0. Summary

**What I would build**, in this order:

1. **What C itself is getting, first.** Typed handles in place of `void*` (`http_response`, `http_request`,
   `json_doc`, ...: C's own idiom of a pointer to an incomplete struct), `defer` (ISO/IEC TS 25755, which clang 22
   implements), and C2y's `if` with a declaration. None of it is FMinus's own, and none of it touches the runtime
   (§3).
2. **`serve`, `route` and `reply`**: a statement that takes the requests waiting for its routes, each route a block
   with typed parameters the request fills, and `reply`, which answers and leaves the route as `return` leaves a
   function. A route that can end without a reply is a compile error, so "a request taken must be answered" stops
   being a rule of the manual (§5).
3. **A JSON literal, `json { "gain": gain, "taps": taps }`, which is an argument and never a value**, as a string
   literal is today: a reply, a POST and a file write take one and write it straight into their own buffer. No JSON
   value exists, so nothing allocates, and the audio thread keeps its replies (§6).
4. **`string`, a view**: a pointer and a length, read-only, over bytes that something else owns (a request, a
   document, a literal). And **`{name}` holes in the three kinds of address** (a URL, a route, a file's name), filled
   from a variable and escaped for their place (§4).
5. **A module read from and written as JSON**, by its fields' names: `json_read(doc, "", &preset)`, and a module as
   a JSON literal's value (§6).
6. **`within (ms) { ... }`**, a deadline around everything a block starts, all of it ended where the block ends, as a
   `parallel` block's statements are; and **`when (condition) statement`**, which runs once each time its condition
   becomes true, for the audio thread, where waiting can only mean looking again next block (§7).

**What I would not build:** `async` and `await` (§7.3), an operator that returns on failure (§3.4), closures or
callbacks, a string type that owns or concatenates, a JSON value type, raw string literals, and a notation of its own
for the networking parts.

**The six things that decide the design:**

- **Syntax is not what is wrong with most of it.** Of the four programs of §8, three are within a few lines of
  today's length, and one is longer. Only the routing shrinks (46 lines to 25). What the statements buy is what can
  no longer go wrong: a request left unanswered, a handle left open on an early return, a number read from the wrong
  `%d`, a quote left unescaped. And three things that cannot be written today at all: a preset with a name, a JSON
  file, and eight requests at once in an array.
- **Sinks, not values.** Every place text or JSON is *made* is a statement or a builtin's argument that writes into a
  buffer it already owns (a reply's slot, a request's body, a file). So the language needs no string builder and no
  JSON tree, and the question "does this allocate on the audio thread" never comes up.
- **A template is a literal with holes, and the host's lists survive it.** `"https://example.com/presets/{name}.json"`
  is still one literal the host reads before anything runs, as `https://example.com/presets/*.json`. Every permission
  system surveyed (Deno, WASI, browser extensions, Landlock) declares patterns and checks at run time; FMinus is the
  only one that derives the list from the program's text, and typed, escaped holes keep that (§4.3).
- **C is moving FMinus's way on two of these and not on the rest.** `defer` is an approved committee draft with a
  shipping implementation; `if` declarations are in the C2y draft; a paper proposes `switch` on strings. C has no
  string view, no raw string, no pattern matching, and nothing for routes or JSON: those are FMinus's own, as modules
  are.
- **The audio thread halves every answer.** A statement that waits is two statements: one for code that may block,
  one for code that may not. `serve (server)` and `serve (server, 100)`, `when` and `within`, are that split, which
  `http_next` and `http_accept` already are.
- **The grammar takes all of it.** Each statement was added to a copy of `Parser.y` and given to Bison 3.8.2: no
  conflict, alone or together (496 states against 400), and the programs of §8 parse (§2).

**What this is not:** nothing was lowered, so nothing here was run. The prototype parses and no more.

**Decisions that are yours** (§11 has each with its reasons):

| Decision | What I would choose | The alternative |
|---|---|---|
| How far from C | C's own additions, then five statements of FMinus's (`serve`, `route`, `reply`, `within`, `when`) | builtins only (§5.1, §7.1): no new syntax at all |
| Text | `string`, a read-only view; `char` arrays to keep one | C's `const char*`, NUL-terminated; or no text, as now |
| Its name | `string` (breaks none of the tree's programs) | `text` (breaks three) |
| Holes | `{name}`, in URLs, routes and file names | `%s` beside `%d`, as C's `printf` |
| Routes | blocks inside a `serve` statement | members of a module, like methods (§5.4) |
| A wrong parameter | a path hole that is no number matches nothing (404); a query value missing, malformed or out of range is 400 | all 404, or all the program's own |
| JSON's numbers | the shortest digits that read back the same; `nan` and `inf` as `null` | `%.9g`; or a NaN is an error the reply reports |
| Unknown members when reading | ignored | refused |
| An address built whole at run time | not in the first version | `http_get(string)`, listed for the host as `*` |
| The handle types' names | `http_response`, `http_request`, `http_server`, `json_doc`, `xml_doc`, `file_content`, `file_listing`, `fft_plan` | yours |

## 1. What is wrong today

Read against the tree's own programs ([examples/remote/remote.fm](../../../examples/remote/remote.fm),
[examples/fetch/fetch.fm](../../../examples/fetch/fetch.fm), [examples/kit/kit.fm](../../../examples/kit/kit.fm)) and
three written for this note (§8 and appendix C):

**Handles and lifetimes.**

- Eight kinds of thing are a `void*`: a request made, a request received, a server, a JSON document, an XML document,
  a file, a listing and an FFT plan. A builtin given the wrong one finds out at run time (`HTTP_INVALID`,
  `FILE_INVALID`), which the compiler could have said.
- There are no arrays of pointers, so there is no array of handles: a program that starts eight requests holds eight
  variables, or a module with one field, eight times.
- Every handle is closed by hand on every path. `fetch.fm`'s `finish` is right because it has one path; the service of
  §8 has three branches and three closes.
- A request taken and never answered keeps its slot until the server closes, and sixteen of them stop the server
  ([web.md](../../web.md), "What it buys and costs"). Nothing checks it.

**Routing.**

- A chain of `if (http_is(r, "..."))`, each branch replying by hand. `remote.fm`'s `answer` is 44 lines for four
  routes, six of them the assignments the program is there to make.
- A path's number is read by its index, `http_arg(r, 0)`, apart from the literal that named it.
- A parameter's range is checked by hand and its 400 written by hand, each time, though FMinus already has
  `[[fminus::range]]` for exactly that on a module's parameters.
- The chain ends in `else http_reply(r, 404);`, which `remote.fm`'s own comment says can never run: the library
  answers what no route names. The language cannot express that the chain is complete.

**Data in and out.**

- JSON is written as a `printf` format with every quote escaped:
  `"{\"gain\":%g,\"pitch\":%g,\"level\":%.4g,\"changes\":%d}"`, the names at one end and the values at the other.
- Only a reply formats text. Nothing writes JSON to a file or into a POST, so the data program of appendix C writes
  raw floats and posts raw floats, because that is all it can.
- A document is read one value a call, each with its path and its fallback, and a preset's five fields are five calls.
- There is no text: a preset is a number, never a name; a header cannot be read; a JSON string can only be compared
  with literals (`json_choice`).

**Waiting.**

- `http_wait` waits for one request. Several are waited for one after another, and a timeout for all of them is
  arithmetic the program does.
- On the audio thread, "when it has arrived, use it" is a flag the program keeps and a test it writes
  (`if (pending && http_status(r) != HTTP_PENDING) { pending = false; ... }`).

## 2. What the grammar can take

FMinus's front end needs no LLVM, so it was built here by itself: `src/frontend/` with Bison 3.8.2 and Flex 2.6.4 (the
pinned versions), and a driver of 25 lines that parses a file and prints its errors. A script then adds candidate
tokens and rules to a *copy* of `Parser.y` and `Lexer.l` (appendix D has the rules; the actions build placeholders,
so the prototype parses and means nothing).

| Added to the grammar | States | Conflicts |
|---|---|---|
| nothing: the tree's `Parser.y` | 400 | 0 |
| `defer statement` | 402 | 0 |
| `if (declaration expr) s` and `if (declaration-without-its-semicolon) s`, with `else` | 413 | 0 |
| the handle types and `string`, as type names | 402 | 0 |
| `serve (args) { route "..." (params) attributes block ... }` and `reply args;` | 428 | 0 |
| `json { ... }`, `json [ ... ]`, `json (expr)` as an expression | 428 | 0 |
| `within (expr) block` | 405 | 0 |
| `when (expr) statement` | 405 | 0 |
| `await (expr);`, only reserved (§7.3) | 405 | 0 |
| `for (type name : expr) statement`, C++'s range `for` (not proposed: §5.4) | 408 | 0 |
| **all of the above** | **496** | **0** |
| ... and routes as a module's members, with `serve (args);` (§5.4) | 498 | 0 |

Three variants that were tried, and what Bison said:

| Variant | Result |
|---|---|
| The word `json` as the document's type too (`json doc = json_parse(...);`) | 3 shift/reduce conflicts: at a statement's start the parser must decide, on seeing `json`, whether an empty `const` precedes a type. So the type is `json_doc`, and `json` begins a literal |
| `reply 200 body;` with no comma | 30 shift/reduce and 124 reduce/reduce conflicts: `reply a -b;` reads two ways. So `reply 200, body;` (FMinus has no comma operator to confuse it with) |
| A route with no keyword, `"GET /x" (int k) { ... }` inside `serve` | 0 conflicts. Kept as `route "..."`: a statement that begins with a string reads as a mistake |

**What the new words cost.** A keyword is reserved outright in FMinus, as `parallel` is. The prototype was run over
every program in the tree that today's grammar parses: 103 of the 123 `.fm` files (the rest are tests of errors and of
builds without a feature, and files only an `#include` reads), 60 programs in the manual's C fences and the tests' raw
strings, and 623 in the string literals of the tests, the examples' hosts and the tools.

| Reserved | Of those 786 programs, broken | Where |
|---|---|---|
| `defer`, `route`, `reply`, `json`, `when`, `await`, the eight handle types, `string` | 0 | |
| `serve` | 2 | `remote.fm`'s method `serve`, and a test's function |
| `within` | 1 | a local variable of `examples/wavetable/wavetable.fm` |
| `text`, had the type been named so | 3 | variables named `text` in `json.fm`, `xml.fm` and a unit test |

`serve` is taken by the one program that serves, which says the word is right; it would rename its method. (The
prototype also refused a fourth program for `text`, the attribute `[[ui::text("...")]]`, only because it did not add
its words to `attribute_word`, where C23 reads a keyword as a name; a real change would.)

**A route's parameter needs a rule of its own.** `params` takes no attributes after a name
(`float value [[fminus::range(0, 1)]]` is a syntax error in a function's parameters today, and was in the prototype
until the route's list got its own rule). Nothing else in the grammar had to bend.

## 3. Handles and lifetimes: what C already has

### 3.1 Typed handles

One type a kind of handle, each a pointer to a struct the program cannot see, which is C's idiom for it (`FILE*`; CERT
DCL12-C; Vulkan's `VK_DEFINE_HANDLE`; Win32's `STRICT`, whose documentation says what it buys: "Errors in passing data
types are reported at compile time instead of causing fatal errors at run time"):

```c
http_server   server = http_listen();
http_request  q = http_next(server);                 // one the program received
http_response r = http_get("https://example.com/presets/7.json");    // one it made, and what came back
json_doc      doc = json_parse(http_body(r), http_size(r));
file_listing  kit = file_list("kit/*.f32");
file_content  f = file_read_nth(kit, 0);
```

- **A wrong handle is a compile error**, `http_size(doc)`, where today it is `HTTP_INVALID` at run time. The runtime
  keeps its check: a `void*` still converts to and from each, as C's does, so every program that says `void*` today
  compiles as it did, unchecked as it was.
- **They are array elements.** `http_response r[16];` is what §7.1 needs, and what "no arrays of pointers" forbids
  today.
- **It stays C.** With `typedef struct fminus_http_response* http_response;` in a header, a program means the same to
  a C compiler.
- **They are what the rest hangs on**: a route's parameter that is the request (§5.2), and, later, a check that a
  local handle is closed on every path. Clang has attributes for that (`acquire_handle`, `release_handle`), but their
  only consumer is a static-analyser checker that honours Fuchsia's handles alone; FMinus's compiler knows which of
  its builtins open and close each kind, and could check with no annotation at all. Not proposed for the first
  version.

### 3.2 `defer`

```c
json_doc doc = json_parse(http_body(r), http_size(r));
defer json_free(doc);                    // when the block ends, however it ends
```

This is C's, or about to be. ISO/IEC TS 25755 is an approved committee draft (WG14 N4001, 2026-11-01); WG14 voted
20-2-2 in March 2026 to publish it; clang 22 implements it behind `-fdefer-ts`. What it says, which FMinus would take
whole:

| | TS 25755 |
|---|---|
| Form | `_Defer` and any unlabeled statement; `defer` is a macro of `<stddefer.h>`, or an extra keyword the TS encourages |
| When | on leaving the enclosing **block**, by its end or by `return`, `break` or `continue`; never the function, as Go's |
| Order | the reverse of the order they were reached |
| Values | nothing is captured: a variable is read when the deferred statement runs. A `return`'s value is computed first |
| Refused | `return`, `break` or `continue` out of the deferred statement; a jump into one or over one |
| Cost | none at run time: "all defer blocks can be transformed during translation and require no execution-time coordination or marking" (N3927) |

- **FMinus takes `defer` and `_Defer` both as keywords** (it substitutes no macros, so the header's way is not open).
  No program in the tree has either as a name.
- **The effects need nothing new.** A deferred call is a call of its function: `defer http_close(r);` in a
  `[[clang::nonblocking]]` function is the error `http_close` is there today, at the `defer`.
- **`reply` (§5) is one more way out of a block**, and a deferred statement runs after the reply's values are
  computed, as after a `return`'s: `defer file_close(f); ... reply 200, file_data(f), file_size(f), "...";` is right.
- **One question is FMinus's own:** a call the loop budget or a bounds check stops early. I would run the deferred
  statements, as for a `return`; the TS's rule for `exit` (they do not run) is about a process ending, which this is
  not.

Block scope is every later design's choice (Zig, Swift, Odin, D), and Go's function scope the one the TS's rationale
calls "an unmitigated disaster of corner cases". C# has the other shape, `using (T x = ...) s`, where the type names
its close; with typed handles FMinus could have it too, but `defer` is C's, is one rule, and frees `malloc`'s memory
as well.

### 3.3 `if` with a declaration

C2y's (N3356, adopted in autumn 2024): a declaration in an `if`'s head, in scope for both branches and no further.

```c
if (http_response r = http_get("https://example.com/presets/{preset}.json"); http_wait(r, 10000) == 200)
{
    defer http_close(r);
    ...
}
```

It is small, it is C's, and the grammar takes it (413 states). GCC's test suite has it under `-std=c2y` and clang has
a commit for it; which releases carry it I did not check.

### 3.4 What I would not add: an operator that returns

Zig's `try`, Rust's `?` and Odin's `or_return` leave the function from inside an expression. Go spent seven years on
three such proposals and in June 2025 stopped: "For the foreseeable future, the Go team will stop pursuing syntactic
language changes for error handling", for want of consensus and because a hidden `return` is hard to see and to stop
at in a debugger. FMinus's statuses are C's, plain `int`s a program tests with `if`, and the three statements above
are what makes the test cheap to write: `if (...) reply 400;` and `if (...) return status;` with the cleanup already
deferred.

## 4. Text

### 4.1 The type: a view

Outside C, every language without a garbage collector that the survey read has the same answer: text is a pointer and
a length over bytes someone else owns (`&str`, `[]const u8`, `std::string_view`, Go's, Odin's and D's `string`, Hare's
`str`), and allocation belongs to a second, owning type. What never allocates is the same list everywhere: length,
one byte, a slice, comparison, a `switch` over literals.

```c
string name = http_header(q, "x-preset");      // bytes of the request, until its reply
if (name == "default") ...                     // by content, byte for byte
switch (json_string(doc, "/wave")) { case "saw": ... case "square": ... default: ... }
```

| | `string` |
|---|---|
| Is | a pointer and a length (16 bytes), read-only, no NUL after it, no encoding checked: bytes, UTF-8 by habit |
| Comes from | a literal; a route's parameter; a query's field; a header; a JSON or XML string; a listing's file name |
| Lives | as long as what it points into: a request until its reply, a document until `json_free`, a literal for ever. The rule `http_body`'s pointer has today, no more |
| Reads, all wait-free | `==` and `!=`; `switch` with literal cases; its length; a byte; a slice; a number read from it (`string_float(s, fallback)`), as a query's is today |
| Goes into | a hole of a template (§4.2); a JSON literal's value (§6.1), escaped; a `char` array, copied, to keep |
| Cannot | be written through, joined to another, or built: text is made in sinks (§6), never as a value |

- **Keeping one** needs bytes of the program's own: `char name[32];` in a module, C's array, and
  `int n = string_copy(name, 32, s);`. That brings `char` in as an element type (an 8-bit integer that reads as an
  `int`), and nothing else of C's strings. `string_of(name, n)` is the view back.
- **The audio thread** loses nothing: the effects (and clang's own, and RealtimeSanitizer) are about the heap and the
  lock, and a view has neither.
- **`switch` on a string** has a paper before WG14 (N3895, June 2026: literal cases only, compared code unit by code
  unit, HTTP servers its motivation; its fate unknown), and D, Hare and Go have it. The grammar takes it already: a
  `case`'s value is an expression, and a string literal is one.
- **Audio languages** mostly stop short of this. Faust and Vult have no text; Cmajor's `string` is "essentially a
  token representing a read-only string literal", with "no concatenation or other run-time mutations ... as these
  would require dynamic memory allocation"; Pd, Max and SuperCollider intern symbols. A view is the smallest step past
  Cmajor's that reads what a request says.

**The alternatives.**

| | For | Against |
|---|---|---|
| **No text** (today) | nothing to build; the host's lists are exact | a service cannot name anything, read a header, or return a string |
| **C's `const char*`**, NUL-terminated | it is C; a prototype of a C function with a string works (`int puts(const char*);`) | a piece of a request's path or of a JSON document has no NUL after it, so each read copies or the library writes into its own buffers; a length is a scan; every overrun is undefined; and this is the type the web's input arrives in |
| **A view** (above) | fits every buffer the runtime already has; nothing is copied; wait-free | not C: a C compiler needs a `struct` and a function for `==`; no C function takes one |
| **An owning string**, with `+` | what a script expects | it allocates, so half the language cannot use it; and the sinks of §6 leave it little to do |

C itself offers no fourth. It has no slice or string-view type and none proposed (the WG14 log, 2025 and 2026); a
length-prefixed string was discussed and not adopted (N3210, recounted in N3608); clang's `-fbounds-safety`, which
would spell a view as `char* __counted_by(n)`, "is not yet available for users". So `string` is a deliberate
departure, as `float4` was.

### 4.2 Holes: `{name}` in an address

Three kinds of literal name a place: a URL, a route and a file's name. Each is a format today, its `%d`s filled from
the ints "after every other argument", so `http_post(url, data, bytes, type, k)` puts `k` four arguments from its
`%d`. A hole names its value where it goes:

```c
http_get("https://example.com/users/{user}/presets/{k}.json");    // user a string, k an int: variables in scope
file_write("takes/{session}/take{n}.f32", take, 4 * taken);
route "PUT /presets/{name}" (string name) { ... }                 // here the hole declares: the path fills name
```

- **A hole is an identifier in scope**, an `int`, a `bool`, an enumeration's value or a `string` (Rust's rule for
  `{x}` in its formats: a name, not an expression). In a route it names one of the route's parameters instead (§5.2).
- **What a hole may say depends on where it is, and the runtime escapes or refuses accordingly**: in a URL's path or
  query, percent-encoded, so a string cannot add a `/`, a `?` or a `#`, and a value of `.` or `..` is refused; in a
  file's name, one segment under every rule a segment has today, and never the extension, which the literal still
  writes; never in a URL's scheme or host (a port may be an `int`, as `%d` is now). jq's `@uri "...\(.x)"` is the same
  policy: the literal's bytes as written, the holes' escaped.
- **Braces are free there.** A route's path cannot hold `{` today; a URI cannot hold one unencoded (RFC 3986; RFC
  6570's templates and OpenAPI's paths put their names in them); a file's name can, so `{{` would be a brace, and no
  address literal in the tree has one.
- **`%d` stays**, and means what it did. `%s` beside it is the smaller step, C's own spelling, and keeps the
  arguments apart from their places.
- **Formats that are C's stay C's**: `http_reply_text`'s is `printf`'s and keeps its `%g`; the JSON literal of §6
  replaces most of its uses.

D's interpolated sequences (2024) are the nearest design: the literal lowers to its pieces and its values, with no
string made; C#'s interpolated-string handlers and Swift's interpolation keep the same separation. All three mark the
literal (`i"..."`, `$"..."`); FMinus need not, because a literal is only ever a builtin's argument and the builtin
says what it is.

### 4.3 What the host's lists keep

Today's promise is that every URL, route and file a program can name is a literal the host lists before anything
runs. The survey's finding is that nobody else has it: every system read declares patterns up front, checks each
operation at run time, and has an escape for what was not foreseen (Deno's prompt, an extension's optional
permissions, a wildcard), and most drop the URL's path from what they match (Chrome: "the path is required but
ignored"). FMinus already has the run-time half: each request is checked against the host's patterns when it starts,
whatever the literal said.

| | The list before running | The check at run time |
|---|---|---|
| Today: `"…/presets/%d.json", k` | `https://example.com/presets/*.json` | each request, each redirect, each address |
| A hole of a string: `"…/presets/{name}.json"` | the same line | the same, on the URL as filled in |
| A whole URL in a `string`: `http_get(url)` | `*`: "any URL the policy allows" | the same |

So holes cost the list nothing: a hole is a `*` whether an int or a string fills it. Only an address built whole at
run time does, and it shows in the list as what it is, for a host to refuse. Deno's loader is the precedent: an
`import()` of a literal is resolved before the program runs, one of a variable is checked when it runs. And Go's
`safehtml` the pattern: a URL "can only be instantiated from compile-time constant string literals", and run-time data
joins it through a constant format whose holes are escaped. None of §8's programs needed a whole URL from a string,
and I would leave it out of the first version.

## 5. Routing

### 5.1 The step that needs no syntax

`scanf`'s way, which parses today: `if (http_is(r, "POST /osc/%d/gain", &k))`. It ends `http_arg` and nothing else;
the chain, the range checks and the replies stay.

### 5.2 `serve`, `route`, `reply`

`remote.fm`'s first route today, and all four as proposed:

```c
if (http_is(r, "POST /gain"))                                    // POST /gain?value=0.5
{
    const float value = http_query_float(r, "value", -1.0f);
    if (value >= 0.0f && value <= 1.0f)
    {
        gain = value;
        changes += 1;
        http_reply(r, 204);
    }
    else
        http_reply(r, 400);
}
else if ...
```

```c
void process(float* out, int n) [[clang::nonblocking]]
{
    serve (server)                                                   // wait-free: each request waiting, or none
    {
        route "POST /gain" (float value [[fminus::range(0, 1)]])     // POST /gain?value=0.5; out of range: 400
        {
            gain = value;
            changes += 1;
            reply 204;
        }
        route "POST /pitch" (float value [[fminus::range(20, 20000)]])
        {
            pitch = value;
            changes += 1;
            reply 204;
        }
        route "POST /note/{note}" (int note [[fminus::range(0, 127)]])   // POST /note/69: A4, 440 Hz
        {
            pitch = 440.0f * expf(0.05776227f * (float)(note - 69));
            changes += 1;
            reply 204;
        }
        route "GET /state"
        {
            reply 200, json { gain, pitch, level, changes };         // {"gain":0.25,"pitch":440,...}
        }
    }
    // ... the block, with gain and pitch as they now are
}
```

**`serve (server)`** takes each request now waiting *for one of its routes* and runs that route's block: wait-free,
what the `for` over `http_next` is today. **`serve (server, ms)`** waits up to `ms` when none is waiting: it blocks,
as `http_accept` does, and is refused in a nonblocking function. The routes are blocks of the function they are in,
so they see its locals and its module's fields, and its effects are theirs: no function is made and nothing is called
back.

**`route "METHOD /path" (parameters) { ... }`**:

| A parameter | Is filled from | When the request does not fit |
|---|---|---|
| named in a `{hole}` of the path, an `int` | that segment, digits as `%d` takes them today | the route does not match: 404, from the library, as now |
| named in a hole, a `string` | that segment, whose bytes the server already holds to letters, digits, `-`, `.`, `_` and `~` | |
| any other, an `int`, `float`, `bool` or `string` | the query's field of its name | missing, or not a number: 400. With `[[fminus::default(x)]]`, missing is `x` |
| with `[[fminus::range(a, b)]]` | | outside it: 400 |
| an `http_request` | the request itself, for `http_body`, `http_size` and a header | |

The 400s are the compiled code's, before the block runs, and wait-free. The split between 404 and 400 is the one the
survey found everywhere: a type in the path is part of matching (ASP.NET, Flask, Rocket: ASP.NET's manual says its
constraints "are used to disambiguate similar routes, not to validate the inputs"), a parameter's conversion is
validation (axum, FastAPI). Ballerina, the one language with routes in its grammar, answers 500 for a segment that
fails its type, which is the mistake to avoid.

**`reply status;`** or **`reply status, body...;`** answers the route's request and leaves the route.

- Its arguments are today's three builtins' without the request, `reply 204;`,
  `reply 200, table, 8192, "application/octet-stream";` and `reply 200, "text/plain", "%d voices", n;`, or a JSON
  literal (§6.1).
- **Every path through a route must reach a `reply`**: FMinus already refuses a non-void function that can reach its
  end, and this is that analysis with another word. `return`, and `break` or `continue` of a loop outside, are not ways
  out of a route.
- So a request is answered exactly once, by construction. The survey's name for the rule is a linear obligation
  (Austral's types enforce such; clang's attributes cannot, in C); here two rules of control flow give it without a
  type system.
- A reply the library refuses (`HTTP_LIMIT`: the body is over the host's `maxReplyBytes`) becomes a 500, so the
  request is still consumed.

**What the compiler now knows, and the host with it.**

- **Overlap is a compile error.** Two routes of a program that one request could match, neither more specific, is
  refused where the second is written. Go's `ServeMux` has the rule ("most specific wins"; a literal segment before a
  hole) and panics when a conflicting pattern is registered; of everything surveyed only Yesod checks at compile
  time, because only there is the table text the compiler reads. FMinus's is. Today's chain takes the first that
  matches, silently.
- **`Program::webRoutes()` gains each route's parameters**: name, type, path or query, range, default. A host can
  build a control page or an OpenAPI description from a program it has not run.
- **Which thread answers which route.** Today the thread that calls `http_next` takes the oldest request whatever its
  route, so a program cannot answer `POST /gain` on the audio thread and `PUT /preset`, which must parse JSON and so
  allocate, on a loader's. A `serve` takes its own routes' requests only, so it can.

### 5.3 What it lowers to

The loop and the chain of today, with the checks written out: `http_next` or `http_accept`, `http_is` per route (the
holes as `%d`s), the query's builtins, the reply's builtins. With one `serve` in a program it needs nothing of the
runtime. With two, taking by route set is one new function: the library already matches each request against the
program's route table to answer 404 and 405, so a slot can carry which route it matched.

### 5.4 The alternatives

| | What it looks like | Why not |
|---|---|---|
| **Routes as members of a module**, with `serve (server);` to dispatch | `module Remote { route "POST /gain" (float value) [[clang::nonblocking]] { ... } };` | Ballerina's shape, and it parses (498 states). But each route is then a function, the split by thread needs two modules, and a route cannot see a local. A `serve` block is control flow, like a `switch` |
| **`switch` on the request** | `switch (r) { case "POST /gain": ... }` | parses today; but cases fall through, have no scope for parameters, and nothing ties a `break` to a reply |
| **C++'s range `for`** over what waits | `for (http_request q : server) ...` | parses (408 states); only renames the loop |
| **A separate file of routes** | Play's `conf/routes`; OpenAPI first | what was asked for is statements; and the program's text is already the route table |

## 6. Data in and out

### 6.1 A JSON literal is an argument, never a value

```c
reply 200, json { "gain": gain, "pitch": pitch, "wave": "saw", "taps": [a, b, c] };
reply 200, json { gain, pitch, level, changes };                  // a name alone: its own key, as in JavaScript
http_response sent = http_post("https://api.example.com/summaries", json { "days": day });
file_write("out/summary.json", json { "days": day });
file_write("presets/{name}.json", json (preset));                 // one value: a module is an object of its fields
```

The rule is the one a string literal has today, word for word: the grammar takes it as an expression, and the code
generator takes it only as the argument of a builtin (or of `reply`) that writes it. Each writes straight into the
buffer it already has: a reply's slot, a request's body, a file's bytes.

| A value that is | Is written as |
|---|---|
| a `float` or a `double` | the shortest digits that read back the same value (`std::to_chars`, which the reply's formatter already uses); `nan` and `inf` as `null`, JavaScript's rule, since JSON has neither |
| an `int`, a `bool` | `-7`, `true` |
| a `string` | a JSON string, escaped |
| an array, a `float4` | an array |
| a module | an object of its fields in their order, each under its name or its `[[json::name("...")]]`; a pointer or a handle is left out |
| an enumeration's | its constant's name, `"SAW"` |
| an array of modules | an array of objects |

- **Nothing allocates.** The survey found heap-free JSON writing documented for one family only: a record walked by
  compile-time reflection and streamed into the caller's buffer (Zig's `std.json.Stringify` over a fixed writer,
  Rust's `serde-json-core`). The macro and builder forms (`serde_json::json!`, nlohmann's initialiser lists) build a
  tree first. This is the first family with JSON's own notation on top, and the compiler knows every key when it
  compiles.
- **On the audio thread a reply's literal is what `http_reply_text` is now**: bytes into the slot's buffer, bounded
  by it.
- **Of scalars alone it lowers to today's format**, `{"gain":%g,...}` made by the compiler, plus one conversion for
  the shortest digits. Strings, arrays and modules need the formatter to learn each.
- **Why JSON's notation and not C's.** C has a record literal, `{ .gain = gain, .taps = { a, b, c } }`, and it would
  read as C. But the keys are JSON's to choose (`"first name"`, `"content-type"`), and a page's author reads the
  reply beside the code that wrote it.
- **Why the word `json`.** `{` begins a block, so a literal needs a first token, and the type cannot share it (§2).
- **It is what a raw string would have been for.** C# writes JSON in `"""` literals with `$$` before them for the
  holes; C23 has no raw string and no paper proposes one for C2y. A literal the compiler reads is checked, key by
  key and value by value, where a raw string is text until it is sent.

### 6.2 A module read from a document

```c
module Preset
{
    float gain [[fminus::range(0, 1)]];
    float cutoff [[fminus::range(20, 20000)]] [[fminus::default(1000)]];
    enum Wave wave;                                  // "wave": "SAW"
};

Preset preset;
if (!json_read(doc, "", &preset))                    // a field missing, of another kind, or out of its range
    reply 400;
```

A builtin, no syntax: the compiler knows a module's fields, as it knows them for the host today, and gives the
runtime a table of names, kinds and offsets, so a document is walked once (five `json_float`s walk it five times).
`json_read` fills each field it finds, of the right kind and in its range, and says whether every field without a
default was there. Every design surveyed binds this way (serde, Zig, Swift's `Codable`, Go, C# with generated code);
they differ in three defaults, each a decision:

| | I would choose | Others |
|---|---|---|
| A member no field names | ignored (serde, Go, C#) | refused (Zig, Kotlin) |
| A field the document lacks | its `[[fminus::default]]`; without one, `json_read` is false | the same everywhere |
| The name | the field's, or `[[json::name("first name")]]` | an attribute or a tag everywhere; C++26's reflection papers use `[[=serde::rename("first name")]]` |

The attributes are already the language's: `fminus::range` and `fminus::default` are a parameter's today, and a
field's attribute of another namespace is passed to the host as written.

### 6.3 What stays

`json_float` and the others, for one value; `json_floats`, for an array of unknown length; `bytes_floats`, for a
body that is no document. XML reading is as it is; an XML writer was not looked at.

## 7. Waiting

### 7.1 In code that may block: `within`

```c
http_response r[MOST];
within (10000)                                   // all of them at once; none is still running after the block
{
    for (int k = 0; k < packs; ++k)
        r[k] = http_get("https://example.com/packs/{k}.json");
}
// here every r[k] has its final status: 200, a failure, or HTTP_TIMEOUT
```

Every request started while the block runs, in it or in a function it calls, has ended where the block ends: the
closing brace waits for them, until the deadline, and cancels what is left. A wait inside it (`http_wait(r, -1)`,
`serve (server, -1)`) returns when the deadline does. It blocks, so it is refused in a nonblocking function.

- **It is `parallel { }`'s promise for requests**: "either has finished, every iteration or statement, where it
  ends". Structured concurrency has one rule in every design surveyed (Trio, Kotlin, Swift, Java, Cilk, OpenMP): a
  child lives in a lexical block and the block does not end before it. FMinus has it for threads already.
- **The deadline is a scope, not a parameter.** That is Trio's `move_on_after`, Kotlin's `withTimeout` and Céu's
  `watching 1s do ... end`: it covers whatever is inside and nothing has to be passed down.
- **It is a statement over a block, so it needs no closure.** "All of these" and "the first of these" are statements
  in Go (`select`), Céu, Esterel and Blech, and library calls over futures everywhere else (`Promise.race`,
  `Task.WhenAny`); only the first kind fits a language without closures.
- **It needs the runtime**: a deadline scope of the calling thread that `http_get` joins and `http_wait` reads.
- **The builtins-only version** is `http_wait_all(r, n, ms)` over an array of typed handles: it waits, and leaves
  what did not finish running.
- **"The first of these"** is not proposed; none of the four programs wanted it.

### 7.2 In code that may not: `when`

```c
void process(float* out, int n) [[clang::nonblocking]]
{
    when (http_status(request) == 200)                   // once, on the block the table arrives in
        length = bytes_floats(http_body(request), http_size(request), table, 2048);
    ...
}
```

`when (c) s` runs `s` each time `c` is true having been false the time before: `if (c && !was) s; was = c;`, where
`was` is a `bool` the compiler keeps in the module's instance, one for each `when`. So it is a method's statement, it
is wait-free, and a new request (the condition false again) arms it again.

- **It is the synchronous languages' `await`, reduced to one bit.** Blech (Bosch's, for embedded real-time code)
  defines `await cond` as exactly this thread's kind of waiting: "Once an activity resumes its execution at an await
  statement the condition is checked. If it is true, the control flow proceeds ... Otherwise, the reaction ends
  immediately and the control flow remains at the same location."
- **It is not only the web's.** A rising edge is what a trigger, a gate and a note-on are:
  `when (gate[i] > 0.5f) envelope.trigger();`.
- **Its cost is a hidden field**, which FMinus has none of today: `sizeof` a module grows by a byte a `when`, after
  its declared fields.

It is the smallest of the proposals, and the one I am least sure earns its keyword: the `if` and the flag it replaces
are three lines.

### 7.3 `async` and `await`: not now

What `await` would be here is Blech's: a method that pauses and is resumed by its next call, its place and its locals
kept in the module (an "activity"). The survey says what it would take:

- **The frame must be the front end's.** A module's size is fixed when it is laid out. LLVM's coroutine passes decide
  a frame's layout in `CoroSplit`, after that: `llvm.coro.size` is a constant only then; the returned-continuation
  lowering takes a caller's buffer of a size chosen in advance but "will allocate memory if the buffer is
  insufficient"; and the documentation warns that "Compatibility across LLVM releases is not guaranteed". The designs
  with a frame of known size (Rust's futures, which Embassy puts in statics; Céu and Blech, which emit static C)
  all build the state machine themselves.
- **It colours functions**: one that pauses may only be called by one that does. FMinus has one such colour already,
  and this would be a second.
- **The audio thread gains little.** What may pause there is reading: starting a request allocates and closing one
  locks, so both are refused in a nonblocking function whatever the syntax. "Has it come? Then use it" is `when`.
- **Code that may block does not need it.** It blocks.

It would earn its place if a server had to hold many slow exchanges open at once, which the sixteen slots of a control
surface do not. `await` is reserved in the prototype so that the word is there if it does.

## 8. The four programs

Each parses with the prototype; the service's counterpart is beside it, and appendix C has the other two.

**A device** is §5.2: `remote.fm`'s four routes, 46 lines today (the chain and the loop), 25 as proposed.

**A service**, a store of presets. Today, where a preset can only be a number, and its body is stored as it came:

```c
void answer(void* r)
{
    if (http_is(r, "PUT /presets/%d"))
    {
        void* doc = json_parse(http_body(r), http_size(r));
        const float gain = json_float(doc, "/gain", -1.0f);
        const float cutoff = json_float(doc, "/cutoff", -1.0f);
        if (!doc || gain < 0.0f || gain > 1.0f || cutoff < 20.0f || cutoff > 20000.0f)
            http_reply(r, 400);
        else if (file_write("presets/%d.json", http_body(r), http_size(r), http_arg(r, 0)) != FILE_OK)
            http_reply(r, 500);
        else
            http_reply(r, 204);
        json_free(doc);
    }
    else if (http_is(r, "GET /presets/%d"))
    {
        void* f = file_read("presets/%d.json", http_arg(r, 0));
        if (file_status(f) == FILE_OK)
            http_reply_bytes(r, 200, file_data(f), file_size(f), "application/json");
        else
            http_reply(r, file_status(f) == FILE_MISSING ? 404 : 500);
        file_close(f);
    }
    else if (http_is(r, "GET /presets"))
    {
        void* all = file_list("presets/*.json");
        http_reply_text(r, 200, "application/json", "{\"count\":%d}", file_count(all));
        file_close(all);
    }
    else
        http_reply(r, 404);
}

float main(float seconds)
{
    void* server = http_listen();
    if (http_port(server) <= 0)
        return (float)http_port(server);
    for (int tick = 0; tick < (int)(seconds * 10.0f) && http_port(server) > 0; ++tick)
    {
        void* r = http_accept(server, 100);
        if (r)
            answer(r);
    }
    http_close(server);
    return 0.0f;
}
```

As proposed, where a preset has a name, its fields are checked by their ranges, and what is stored is the preset and
not whatever was sent:

```c
enum Wave { SINE, SAW, SQUARE };

module Preset                                        // what a preset holds, and so what its JSON says
{
    float gain [[fminus::range(0, 1)]];
    float cutoff [[fminus::range(20, 20000)]];
    enum Wave wave;                                  // "wave": "SAW"
};

float main(float seconds)
{
    http_server server = http_listen();
    defer http_close(server);
    if (http_port(server) <= 0)
        return (float)http_port(server);
    for (int tick = 0; tick < (int)(seconds * 10.0f) && http_port(server) > 0; ++tick)
        serve (server, 100)                          // waits up to 100 ms for a request for one of these routes
        {
            route "PUT /presets/{name}" (string name, http_request q)
            {
                json_doc doc = json_parse(http_body(q), http_size(q));
                defer json_free(doc);
                Preset preset;
                if (!json_read(doc, "", &preset))    // a field missing, of another kind or out of its range
                    reply 400;
                if (file_write("presets/{name}.json", json (preset)) != FILE_OK)
                    reply 500;
                reply 204;
            }
            route "GET /presets/{name}" (string name)
            {
                file_content f = file_read("presets/{name}.json");
                defer file_close(f);
                if (file_status(f) != FILE_OK)
                    reply file_status(f) == FILE_MISSING ? 404 : 500;
                reply 200, file_data(f), file_size(f), "application/json";
            }
            route "GET /presets"
            {
                file_listing all = file_list("presets/*.json");
                defer file_close(all);
                reply 200, json { "count": file_count(all) };
            }
        }
    return 0.0f;
}
```

The name goes from the request's path into a file's name, and two things make that safe: a route's segment is held to
letters, digits, `-`, `.`, `_` and `~` before the program sees it, and a file's name is never trusted as text (the
runtime walks it a segment at a time beneath the folder the host granted, and refuses a segment that begins with a
dot).

**A script**, which fetches packs and writes each as raw floats. Today it fetches them one after another (appendix
C); as proposed, at once, with one deadline:

```c
enum { MOST = 16 };

float main(float count)
{
    const int packs = (int)count < MOST ? (int)count : MOST;
    http_response r[MOST];
    within (10000)                                   // all of them at once; none is still running after the block
    {
        for (int k = 0; k < packs; ++k)
            r[k] = http_get("https://example.com/packs/{k}.json");
    }
    int written = 0;
    for (int k = 0; k < packs; ++k)
    {
        defer http_close(r[k]);
        if (http_status(r[k]) != 200)
            continue;
        json_doc doc = json_parse(http_body(r[k]), http_size(r[k]));
        defer json_free(doc);
        float table[2048];
        const int n = json_floats(doc, "/table", table, 2048);
        if (n > 0 && n <= 2048 && file_write("out/pack{k}.f32", table, 4 * n) == FILE_OK)
            written += 1;
    }
    return (float)written;
}
```

**Data work**: a day of readings from each of eight sensors, its mean and its peak, written and posted. Today the
summary can only be raw floats (appendix C); as proposed it is JSON, an array of modules:

```c
enum { SENSORS = 8, SAMPLES = 1440 };

module Day { int sensor; float mean; float peak; };

float main()
{
    http_response r[SENSORS];
    within (30000)
    {
        for (int k = 0; k < SENSORS; ++k)
            r[k] = http_get("https://api.example.com/sensors/{k}/day.json");
    }
    Day day[SENSORS];
    for (int k = 0; k < SENSORS; ++k)
    {
        defer http_close(r[k]);
        json_doc doc = json_parse(http_body(r[k]), http_size(r[k]));     // no body: no document, and no values
        defer json_free(doc);
        float value[SAMPLES];
        int n = json_floats(doc, "/values", value, SAMPLES);
        n = n < SAMPLES ? n : SAMPLES;
        double sum = 0.0;
        for (int i = 0; i < n; ++i)
        {
            sum += value[i];
            day[k].peak = fmaxf(day[k].peak, fabsf(value[i]));
        }
        day[k].sensor = k;
        day[k].mean = n > 0 ? (float)(sum / n) : 0.0f;
    }
    file_write("out/summary.json", json { "days": day });                // an array of modules: an array of objects
    http_response sent = http_post("https://api.example.com/summaries", json { "days": day });
    defer http_close(sent);
    return http_wait(sent, 10000) == 201 ? 1.0f : -1.0f;
}
```

| Program | Lines today | As proposed | What changed besides |
|---|---|---|---|
| The device's routes | 46 | 25 | a wrong value is refused by its range; no route can go unanswered |
| The service | 47 | 44 | presets have names; every field is checked; closed on every path |
| The script | 19 | 25 | every request at once, one deadline |
| The data work | 34 | 33 | JSON out, where raw floats were all there was |

## 9. What each proposal keeps

You said none of these is sacred. Most of the proposals keep all four anyway, and the table says where one does not.

| | The host's lists before running | The audio thread | A valid C program | Today's runtime is enough |
|---|---|---|---|---|
| Typed handles | the same | the same | yes, with a header of typedefs | yes |
| `defer` | the same | a rewrite when compiled: nothing at run time | TS 25755's (clang 22, `-fdefer-ts`) | yes |
| `if` with a declaration | the same | the same | C2y's | yes |
| `serve`, `route`, `reply` | more: each route's parameters and ranges | wait-free as the chain is; one reply, checked | no: FMinus's own | yes for one `serve`; one function for two |
| A JSON literal | the same | writes into the reply's buffer, as now | no | scalars yes; strings, arrays and modules need the formatter |
| `string` | the same | every read wait-free | no: a `struct` at best | new reads (a header, a JSON string) |
| `{name}` holes | the same pattern, a `*` a hole | no change | no: `%s` would be | escaping |
| `json_read` | the same | wait-free, and one walk of the document | the call is C | one function, and a table the compiler makes |
| `within` | the same | refused in a nonblocking function | no | a deadline scope |
| `when` | the same | wait-free; one hidden `bool` | no | yes |
| A whole URL in a `string` (not proposed) | **a `*`: any URL the policy allows** | no change | no | little |

## 10. Order, and what each costs the compiler

**The order I would build in**, each step useful without the next:

1. **Typed handles, `defer`, `if` declarations.** The front end and the code generator only. `defer` is the work: a
   block's exits (its end, `return`, `break`, `continue`) each run its deferred statements, in `src/ir/`'s
   statements, as clang's cleanup scopes do. FMinus has no `goto`, so the TS's hardest constraints have nothing to
   check.
2. **`serve`, `route`, `reply`, with numbers**, and **the JSON literal of scalars**. Front end and code generator:
   routes with parameters into the route table `src/ir/WebServer.cpp` already makes, the overlap check, the reply
   check (the reachability analysis the missing-`return` error has), a literal flattened to a format. `webRoutes()`
   gains parameters, so `FMinus.h` changes.
3. **`string`, holes, strings in routes, queries, headers and JSON.** The largest step: a type, `char` arrays to keep
   one, the templates in `src/ir/Strings.cpp`, escaping in the runtime and the web library, and new reads.
4. **Modules and JSON**: `json_read`, a module and an array in a literal, `file_write` and `http_post` taking one.
5. **`within`**, then **`when`**.

**Every step also touches** `Lexer.l` and `Parser.y` (appendix D), `Ast.h` and its `dump`, the README's language table
and its list of where FMinus departs from C, [web.md](../../web.md) and [files.md](../../files.md), a recipe in
[extending.md](../../extending.md), the lit tests and the unit tests, and `editors/vscode`'s grammar for the new
words. The examples that would be rewritten are `remote/`, `fetch/` and `kit/`, and `remote.fm`'s `serve` needs a new
name.

**Not touched:** the circuit compiler (none of the names its modules use is a new word), the Python package, the
policies, and the web library's and the file interface's enforcement, which is the same code whatever the syntax
above it.

## 11. Decisions that are yours

1. **How far to go.** Step 1 is C and commits FMinus to nothing of its own. Steps 2 to 5 are five keywords, a type
   and a literal. Each stops being a subset of C in the way modules did.
2. **Text: a view, C's `char*`, or none** (§4.1). I recommend the view, and `char` only as what an array holds.
3. **`string` or `text`.** `string` is what Go, Odin, D and Cmajor call it and breaks nothing; `text` says plainly
   that it is not C's, and takes a name three of the tree's programs use.
4. **`{name}` or `%s`** (§4.2). `{name}` is what Go's router, OpenAPI and RFC 6570 write, and puts a value where it
   goes; `%s` is C's.
5. **Routes in a `serve` block, or as a module's members** (§5.4).
6. **404 or 400** for a parameter that does not fit (§5.2).
7. **JSON's edges**: a NaN as `null`, an enumeration as its name, a member no field names ignored (§6).
8. **`when`**: whether three lines are worth a keyword and a hidden field (§7.2).
9. **A whole address from a string**: never, or listed as `*` (§4.3).
10. **The names**: the handle types', and `serve` and `within`, which between them break three programs of the tree.

## 12. What was measured, what was read, and what was neither

**Measured here** (Linux x86-64; scratch files, not part of the tree):

| What | Result |
|---|---|
| Flex 2.6.4 from the pinned archive | its SHA-256 is the pin's; built with clang 18.1.3 |
| `src/frontend/` built by itself (Bison 3.8.2, Flex 2.6.4, `-std=c++23`), no LLVM | builds; 400 states, no conflict |
| That parser over the tree's 123 `.fm` files | 103 parse; the 20 that do not are error tests, tests of builds without a feature, and files only an `#include` reads |
| Each candidate added to a copy of the grammar | §2's table: no conflict, alone or together |
| The three variants | §2: 3 conflicts; 30 and 124; none |
| The reserved words over 786 programs | §2: `serve` 2, `within` 1; `text` 3 |
| The programs of §5.2 and §8, as proposed, and two more (a loader with `when` and an `if` declaration; routes as members, with their two rules added) | each parses with the prototype; each is a syntax error to the tree's grammar |
| The three programs of today's syntax written for this note | each parses with the tree's grammar |
| Every C block of this report, taken from the file | each parses: today's syntax with the tree's grammar, the proposed with the prototype (a fragment inside a function, a `...` read as an empty statement, and the two blocks that mix a declaration with statements in their pieces) |
| An address literal with a brace in it, anywhere in the tree | none |
| Line counts | §8's table: lines that are not blank or a comment |

**Not done:**

- **Nothing was lowered.** The prototype's actions build placeholders. No proposed program was compiled or run, no
  effect was checked, no error message was written, and the claim that a `serve` block lowers to today's builtins is
  an argument (§5.3), not a test.
- **Today's three programs were parsed, not compiled.** FMinus was not built here (a first configure builds LLVM).
  They follow `remote.fm`, `fetch.fm` and `kit.fm`, which are tested, but a mistake of types in them would have gone
  unseen.
- **No cost was measured**: of `serve` against the chain, of a JSON literal against a format, of `json_read` against
  five reads.
- **TS 25755's `defer` was not tried**: clang here is 18, and it came in 22.

**Read at its source** (2026-11-12, by five surveys run for this note; the appendices have what each rests on):
WG14's papers and minutes, clang's and GCC's release notes and sources, each language's reference or specification,
LLVM 23.1.0's coroutine documentation, the RFCs, and the permission systems' own documentation. I checked the surveys
against each other and against what I already knew of each design; I did not open every source again myself.

**Neither read at a primary source nor tested:**

- Whether ISO has published TS 25755, or balloted it: iso.org refused the request. No GCC release has `defer`; its
  patch's fate is unknown.
- Which releases of GCC and clang carry C2y's `if` declarations: known only from their repositories' tests and
  commits.
- What became of N3895 (`switch` on strings) at WG14.
- That C++26 was published: a failed ballot in October 2026 is reported at second hand.
- Criticism of Ballerina's routes at a primary source; what the frameworks other than Go answer for a wrong method.
- Java's structured concurrency being final in JDK 28: openjdk.org refused every request.
- Any published guidance on coroutines on an audio thread: none was found. Clang 23's function effects say nothing of
  them.

## Appendix A: the survey of cleanup, status and handles

| Fact | Source |
|---|---|
| `defer` is a Technical Specification, not part of the C2y draft; latest draft N4001, "Approved Committee Draft", 2026-11-01; the syntax and semantics of §3.2 | [WG14 N4001](https://www.open-std.org/jtc1/sc22/wg14/www/docs/n4001.pdf) |
| WG14's poll to publish, 20-2-2; "Clang 22 shipped with an implementation of this TS, and received positive feedback" | [WG14 N3954](https://www.open-std.org/jtc1/sc22/wg14/www/docs/n3954.htm), minutes of March 2026 |
| Why block scope, why nothing is captured, and no run-time cost | [WG14 N3927](https://www.open-std.org/jtc1/sc22/wg14/www/docs/n3927.htm), §4.1, §4.4, §4.6 |
| Clang 22.1.0: "Implemented the defer draft Technical Specification (WG14 N3734); it is enabled in C mode by passing `-fdefer-ts`"; only `_Defer` is a keyword there | [Clang 22.1.0 release notes](https://releases.llvm.org/22.1.0/tools/clang/docs/ReleaseNotes.html) |
| GCC: a patch of August 2025, in no release | [gcc-patches](https://gcc.gnu.org/pipermail/gcc-patches/2025-August/693318.html); [GCC 16 changes](https://gcc.gnu.org/gcc-16/changes.html) |
| Go's `defer` is the function's, and takes its arguments when reached; Zig, Swift, Odin and D are the block's | each language's reference; [Go blog](https://go.dev/blog/defer-panic-and-recover) |
| C#'s `using`, Java's try-with-resources and Python's `with` are each a rewrite to `try`/`finally` with a close the type names | Microsoft Learn; the Java Language Specification; the Python reference |
| Go, 2025-06-03: no more syntax for error handling, and why | [go.dev/blog/error-syntax](https://go.dev/blog/error-syntax) |
| WG14 has adopted nothing for error handling; the live papers are N3766 and N3976 | [the WG14 document log](https://www.open-std.org/jtc1/sc22/wg14/www/wg14_document_log.htm) |
| "Exactly once" as a compile error: Austral's linear types. Rust's ownership is "at most once"; `[[nodiscard]]` is advice | austral-lang.org; the Rust reference |
| Clang's handle attributes work in C; their one consumer is `fuchsia.HandleChecker`, for Fuchsia's tags | [Clang attribute reference](https://clang.llvm.org/docs/AttributeReference.html#handle-attributes); `FuchsiaHandleChecker.cpp` |
| Opaque types as pointers to incomplete structs, and what they catch | [SEI CERT C, DCL12-C](https://wiki.sei.cmu.edu/confluence/display/c/DCL12-C.+Implement+abstract+data+types+using+opaque+types); `vulkan_core.h`; [Win32 `STRICT`](https://learn.microsoft.com/en-us/windows/win32/winprog/enabling-strict) |
| C2y's `if` declarations: N3356, adopted at the autumn 2024 meeting; refined by N3388, N3473 and N3580 | [N3356](https://open-std.org/jtc1/sc22/wg14/www/docs/n3356.htm), through a search's summary of the papers and of GCC's and clang's repositories |

## Appendix B: the surveys of routing, data, text and waiting

**Routing.**

| Fact | Source |
|---|---|
| Ballerina: the method is the accessor, the path the function's name, typed segments in brackets, `resource function get data/[int age]/[string name]()`; a path is identifiers, "not string literals"; a segment that fails its type is 500 | [Ballerina's HTTP specification](https://ballerina.io/spec/http/), 2.3.1 to 2.3.3 |
| Go's `ServeMux`: `[METHOD ][HOST]/[PATH]`, `{name}`, `{name...}`, `{$}`; the most specific pattern wins whatever the order; a conflict panics at registration; 405 with `Allow` from the mux; `GET` matches `HEAD` | [pkg.go.dev/net/http](https://pkg.go.dev/net/http#ServeMux); [the routing post](https://go.dev/blog/routing-enhancements) |
| Crow: `CROW_ROUTE(app, "/add/<int>/<int>")`, placeholders by position, checked against the handler when compiled | [Crow's guide](https://crowcpp.org/master/guides/routes/) |
| ASP.NET: "Don't use constraints for input validation ... constraints are used to disambiguate similar routes" | Microsoft Learn, "Routing in ASP.NET Core" |
| Overlap: at compile time in Yesod alone; at registration in Go, axum and Rocket; at the request in ASP.NET; never (first match) in FastAPI and Play | each framework's documentation |
| No standard types a placeholder: RFC 6570 expands and does not match; URLPattern has `:name` and regular expressions; OpenAPI 3.2 has `{name}` and the type apart | RFC 6570; [urlpattern.spec.whatwg.org](https://urlpattern.spec.whatwg.org/); [OpenAPI 3.2.0](https://spec.openapis.org/oas/v3.2.0.html#path-templating) |
| Pattern matching is not in C++26: P2688R6 (2026-09-22) "now targets C++29". WG14 has no such paper | P2688R6; the WG14 document log |
| N3895, "Switch on Strings": a `switch` on a pointer to characters, every case a string literal | [the WG14 document log](https://www.open-std.org/jtc1/sc22/wg14/www/wg14_document_log.htm), 2026-06-07 |
| Wasp gave up its own language for routes in 0.24, for TypeScript | wasp.sh's documentation |

**Data.**

| Fact | Source |
|---|---|
| JSON written with no heap: Zig's `std.json.Stringify.value` over `std.Io.Writer.fixed`; Rust's `serde-json-core`. `serde_json::json!`, Kotlin's `buildJsonObject` and nlohmann's lists build a tree | Zig's `lib/std/json`; docs.rs; each library's README |
| JavaScript writes `NaN` and `Infinity` as `null`; its object literal has names alone as members | MDN, `JSON.stringify` and "Object initializer" |
| Reading: Zig refuses an unknown member and a missing field without a default; serde, Go and C# ignore the unknown unless asked | Zig's `static.zig`; serde.rs; Go's `encoding/json/v2` |
| C++26's annotations use `[[=serde::rename("first name")]]` as their example | the C++26 reflection papers (P2996, P3394) |
| Literal pieces kept apart from the values, when compiled: C#'s interpolated-string handlers, Swift's interpolation, D's sequences (DMD 2.108.0). At run time only: Python's t-strings (3.14), JavaScript's tagged templates | Microsoft Learn; docs.swift.org; dlang.org; PEP 750; MDN |
| jq's `@uri "...\(.x)"` escapes the holes and not the literal | jq's manual |
| C23 has no raw string literal and no paper proposes one for C2y; `#embed` is C23's, in GCC 15 and clang 19 | the WG14 document log through N4018; the compilers' release notes |

**Text and auditing.**

| Fact | Source |
|---|---|
| A pointer and a length is the default text of Rust, Zig, Odin, C3, D, Hare, Go and C++'s `string_view`; allocation is another type's | each language's reference |
| C has no slice, span or string view, and none proposed in 2025 or 2026; N3210's string was thought to cost too much a string | the WG14 document log; [N3608](https://www.open-std.org/jtc1/sc22/wg14/www/docs/n3608.pdf) |
| "Upstreaming of `-fbounds-safety` is still in progress, and the feature is not yet available for users" | [clang's `docs/BoundsSafety/index.md`](https://github.com/llvm/llvm-project/blob/main/clang/docs/BoundsSafety/index.md), on main |
| Cmajor's `string`: a token for a literal, with no run-time mutation, "as these would require dynamic memory allocation" | cmajor.dev's language guide |
| Symbols interned and compared by address: Pd, Max, SuperCollider's `Symbol`, JUCE's `Identifier` | each one's documentation or source |
| Go's `safehtml`: a `TrustedResourceURL` "can only be instantiated from compile-time constant string literals, command-line flags or a combination of the two"; `TrustedResourceURLFormatFromConstant` escapes each hole | [pkg.go.dev/github.com/google/safehtml](https://pkg.go.dev/github.com/google/safehtml) |
| Python's `LiteralString` is a checker's, with no run-time existence | [PEP 675](https://peps.python.org/pep-0675/) |
| Deno: an `import()` of a literal is resolved before the program runs; of a variable, checked at run time | docs.deno.com, "Security and permissions" |
| Chrome's match patterns: "For host permissions, the path is required but ignored" | [developer.chrome.com](https://developer.chrome.com/docs/extensions/develop/concepts/match-patterns) |
| Files as a directory's handle with names beneath it: WASI's preopens, Landlock, `unveil` | the WASI filesystem proposal; docs.kernel.org; man.openbsd.org |

**Waiting.**

| Fact | Source |
|---|---|
| Blech: activities may pause and functions may not; `await cond` is checked again each reaction; `cobegin ... with ... end`; every loop in an activity must reach a pause | [Blech's user manual](https://www.blech-lang.org/docs/user-manual/statements/) |
| Céu: `par/and`, `par/or`, and "A watching expands to a par/or with n+1 trails" | [Céu's manual, v0.30](https://ceu-lang.github.io/ceu/out/manual/v0.30/statements/) |
| "All of" and "the first of" are statements in Go, Céu, Esterel and Blech, and library calls elsewhere | each language's reference |
| The deadline as a scope: Trio's `move_on_after`, Kotlin's `withTimeout`, asyncio's `timeout` | each one's documentation |
| Structured concurrency's one rule, in Trio, Kotlin, Swift, Java, Cilk and OpenMP | each one's documentation; N. J. Smith's essay |
| LLVM's coroutines: `llvm.coro.size` "is lowered to a constant" by the passes; a returned-continuation coroutine "will allocate memory if the buffer is insufficient"; "Compatibility across LLVM releases is not guaranteed" | [LLVM 23.1.0, Coroutines](https://releases.llvm.org/23.1.0/docs/Coroutines.html) |
| A frame of a size the front end knows: Rust's futures, Embassy's static tasks; C++ cannot know it during semantic analysis | the Rust async book; embassy.dev; P1362R0 |
| Zig removed `async` and `await` in 0.15.1 and has an `Io` interface since 0.16.0; stackless coroutines are still a proposal in 0.17.1 | Zig's release notes |
| Protothreads and their kin: a `switch` on a saved line; locals are not kept | Dunkels's protothreads; Tatham's "Coroutines in C" |

## Appendix C: the script and the data work in today's syntax

Each parses with the tree's grammar. The script:

```c
float main(float count)
{
    int written = 0;
    for (int k = 0; k < (int)count; ++k)
    {
        void* r = http_get("https://example.com/packs/%d.json", k);
        if (http_wait(r, 10000) == 200)
        {
            void* doc = json_parse(http_body(r), http_size(r));
            float table[2048];
            const int n = json_floats(doc, "/table", table, 2048);
            if (n > 0 && n <= 2048 && file_write("out/pack%d.f32", table, 4 * n, k) == FILE_OK)
                written += 1;
            json_free(doc);
        }
        http_close(r);
    }
    return (float)written;
}
```

The data work:

```c
enum { SENSORS = 8, SAMPLES = 1440 };

float main()
{
    float summary[SENSORS * 2];                      // each sensor's mean, then its peak: raw floats, the only
    int read = 0;                                    // thing a program can write to a file or post
    for (int k = 0; k < SENSORS; ++k)
    {
        void* r = http_get("https://api.example.com/sensors/%d/day.json", k);
        if (http_wait(r, 30000) == 200)
        {
            void* doc = json_parse(http_body(r), http_size(r));
            float value[SAMPLES];
            int n = json_floats(doc, "/values", value, SAMPLES);
            n = n < SAMPLES ? n : SAMPLES;
            double sum = 0.0;
            float peak = 0.0f;
            for (int i = 0; i < n; ++i)
            {
                sum += value[i];
                peak = fmaxf(peak, fabsf(value[i]));
            }
            summary[2 * read] = n > 0 ? (float)(sum / n) : 0.0f;
            summary[2 * read + 1] = peak;
            read += 1;
            json_free(doc);
        }
        http_close(r);
    }
    file_write("out/summary.f32", summary, 8 * read);
    void* sent = http_post("https://api.example.com/summaries", summary, 8 * read, "application/octet-stream");
    const int status = http_wait(sent, 10000);
    http_close(sent);
    return status == 201 ? (float)read : -1.0f;
}
```

## Appendix D: the rules Bison was given

Added to a copy of `Parser.y`, with a token for each new word and the handle types' names and `string` returned as
types, as `float4` is. The actions are left out: each builds an empty statement or the number 0.

```
type        : ... | HANDLE_TYPE | "string"

unlabeled   : ...
            | "defer" statement
            | "if" "(" declaration expr ")" statement                        (and with "else" statement)
            | "if" "(" qualifier type declarator ")" statement               (and with "else" statement)
            | "serve" "(" arg_list ")" "{" route_items "}"
            | "reply" arg_list ";"
            | "within" "(" expr ")" block
            | "when" "(" expr ")" statement
            | "await" "(" expr ")" ";"                                       reserved only

route_items : %empty | route_items route_item
route_item  : "route" string_literal route_params attributes block
route_params: %empty | "(" ")" | "(" route_param_list ")"
route_param_list : route_param | route_param_list "," route_param
route_param : qualifier type pointer IDENTIFIER attributes

expr        : ... | "json" json_object | "json" json_array | "json" "(" expr ")"

json_value  : expr | json_object | json_array
json_object : "{" "}" | "{" json_members "}" | "{" json_members "," "}"
json_members: json_member | json_members "," json_member
json_member : string_literal ":" json_value | IDENTIFIER
json_array  : "[" "]" | "[" json_values "]"
json_values : json_value | json_values "," json_value
```

For routes as members (§5.4): `module_member : ... | route_item` and `unlabeled : ... | "serve" "(" arg_list ")" ";"`.
A real change would also add each new keyword to `attribute_word`, where C23 reads a keyword as a name.
