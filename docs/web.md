# The web library

`FMINUS_WEB` gives FMinus programs an HTTP(S) client: `http_get` and `http_post` start a request and return at once,
`http_wait` blocks until it is done, and the audio thread may read its status, its progress and its body without
waiting. The library is `fminus_web` (`src/web/`), its own static library beside the runtime, on libcurl with Mbed TLS
(Schannel on Windows), and it is off by default. Through it a program reaches only what its host allows: the default
is no network, every URL is a string literal the host can list before anything runs, and the library checks each
request against the host's patterns when it starts, at each redirect and on each connection's address. (The policy is
the library's, not a sandbox: [the host's side](#the-hosts-side) says what it does not cover.)

`FMINUS_WEB_SERVER`, a second option on top of the first, also off by default, gives programs a server: `http_listen()`
listens where the host says (nowhere by default), and the program takes requests and answers them with builtins, its
audio thread among the threads that may, wait-free. Its routes are string literals the host can list before anything
runs, and the library answers everything else itself. [The server](#the-server) is its half of this page.

```c
module Loader
{
    http_response request;
    float table[2048];
    int length;

    void load(int preset)                                      // the host calls it off the audio thread
    {
        request = http_get("https://example.com/presets/%d.json", preset);
        defer http_close(request);                             // however load ends
        if (http_wait(request, 10000) == 200)                  // blocks: refused in a nonblocking function
        {
            json_doc doc = json_parse(http_body(request), http_size(request));
            defer json_free(doc);
            length = json_floats(doc, "/table", table, 2048);
        }
    }
};
```

```cpp
fminus::Program::Options options;
options.web.allow = {"https://example.com/presets/*"};          // nothing listed: no network
auto program = fminus::Program::compile(source, errors, options);
for (const auto& request : program->webRequests())               // every URL, before anything runs
    show(request.method, request.pattern, request.line);         // GET https://example.com/presets/*.json 11
```

Contents: [building it](#building-it), [the builtins](#the-builtins), [the status codes](#the-status-codes),
[within](#within), [the host's side](#the-hosts-side), [the pattern language](#the-pattern-language), [the driver and
executables](#the-driver-and-executables), [threads and lifetime](#threads-and-lifetime), [determinism and offline
rendering](#determinism-and-offline-rendering), [plugins](#plugins-and-entitlements), [the server](#the-server) (its
[building](#building-the-server), [builtins](#the-servers-builtins), [routes and replies](#routes-and-replies), [the
audio thread](#how-a-request-reaches-the-audio-thread), [the host's side](#what-the-host-gives-the-server),
[security](#what-a-request-meets), [the driver and executables](#serving-from-the-driver-and-executables), [threads
and lifetime](#the-servers-threads-and-lifetime)), [how it is built](#how-it-is-built), [what is verified
where](#what-is-verified-where), [what is not built](#what-is-not-built). The research behind it, and the choices it
made, is [research/web/README.md](research/web/README.md), and for the server
[research/web/server.md](research/web/server.md).

## Building it

The library is part of FMinus's build, off by default: `FMINUS_WEB=ON` builds it. The `web` preset is `dev` with it
on:

```bash
cmake --workflow --preset web            # configure, build and test in build-web/
```

Any other tree takes the option too (`cmake --preset release -DFMINUS_WEB=ON`). The first configure with it on
downloads and builds two dependencies into the deps cache, as Bison and Flex are built (`cmake/FMinusWeb.cmake`,
pinned in `cmake/FMinusPins.cmake`): **libcurl 8.23.0**, static, with HTTP and HTTPS and nothing else (no other
protocol, cookies, `.netrc`, alt-svc, HSTS, proxies, authentication, forms, DoH, WebSockets, IDN, compression or
HTTP/2), and on macOS and Linux **Mbed TLS 4.1.1**, its long-term-support branch, with threading on. Windows uses
Schannel, the system's TLS, and downloads no TLS library. Each takes a minute or two to build, once per compiler and
set of options; every later configure, of any tree, finds the install. Both are compiled with their own flags,
hidden visibility (a plugin that links FMinus exports none of their symbols) and position-independent code.

With the option off nothing is fetched or built for it, there is no `fminus_web` target, and `fminus/Config.h` says
`#define FMINUS_HAS_WEB 0`. A program's `http_get` is then still a name FMinus knows: its call is the error
`'http_get' needs the web library, which this build of FMinus leaves out (FMINUS_WEB=OFF)`, and the same for every
`http_` builtin and every status code's name. `__FMINUS_WEB__`, a macro FMinus defines (to 1) only in a build with the
library, lets one source compile both ways:

```c
#ifdef __FMINUS_WEB__
    request = http_get("https://example.com/ir.f32");
#endif
```

`FMINUS_WEB=ON` with `FMINUS_JSON` and `FMINUS_XML` both off is legal (`bytes_floats` and `bytes_pcm16` still decode a
body), and configure says it is probably not what was meant.

The server is a second option, `FMINUS_WEB_SERVER`, off by default and only with `FMINUS_WEB` on (configure stops
otherwise, and says so); the `web` preset turns both on. [Building the server](#building-the-server) has what it
fetches.

## The builtins

| Builtin | Does | Effect |
|---|---|---|
| `http_response http_get(url, ...)` | starts a GET of the URL and returns its request, a handle, at once | allocates |
| `http_response http_post(url, data, bytes, content_type, ...)` | starts a POST of `bytes` bytes at `data` (copied: the buffer is free again when it returns) | allocates |
| `http_response http_post(url, json ..., ...)` | starts a POST of a JSON literal's text, sent as `application/json`: measured, refused over the policy's `maxBodyBytes` (`HTTP_LIMIT`), written into the request's own body ([The JSON literal](#the-json-literal)) | allocates |
| `int http_wait(r, timeout_ms)` | blocks until the request is done or `timeout_ms` has passed (0: not at all; negative: until done, which the policy's timeout bounds), and gives its status then, 0 if still pending | blocks |
| `int http_status(r)` | 0 while pending, the HTTP status once a response arrived, or a failure (below) | wait-free |
| `int http_size(r)`, `const void* http_body(r)` | the body's bytes and its address, once the status is an HTTP status (0 and null before, and after a failure); valid until `http_close`. A NUL follows the last byte, not counted | wait-free |
| `int http_progress(r)` | the body's bytes received so far (of the current hop, while redirects are followed); the size once done | wait-free |
| `void http_cancel(r)` | stops a pending request: afterwards its status is `HTTP_CANCELLED`, or what it ended with first | allocates |
| `void http_close(r)` | cancels it if pending, and frees it and its body | allocates |

- **The URL is a string literal**, a format whose `%d`s are the `int`s after every other argument, as `printf`'s
  are (`http_post(url, data, bytes, type, ints...)`), checked when the program is compiled as clang checks `printf`'s
  ([extending.md](extending.md), recipe 38). Beyond the format, a URL is printable ASCII with no space and no
  backslash (so no CR or LF: nothing a program writes adds a header to a request), its scheme `http` or `https`, and
  it carries no credentials (no `@` before its path); each mistake is an error at its byte. The content type is a
  literal too, printable ASCII, at most 255 bytes (empty: `application/octet-stream`).
- **Or the URL names its values where they go, in holes**: `http_get("https://example.com/users/{user}/presets/{k}.json")`,
  each `{name}` a variable in scope (a local, a parameter, a field, a constant: a name, not an expression), an `int`,
  a `bool` or an enumeration's value (written in decimal, as `%d` is) or a `string`. A literal has `%d`s or holes, not
  both, and a literal with holes takes no ints after it; `{{` and `}}` are a brace, `%%` a percent sign still. No hole
  may be in the scheme or the host; a port may be an `int`'s; the path, the query and the fragment take either. **A
  string is percent-encoded** as it is filled in, every byte but the unreserved ones (`A`-`Z`, `a`-`z`, `0`-`9`, `-`,
  `.`, `_`, `~`) written `%XX`, so it cannot add a `/`, a `?`, a `#` or a `%` of its own; in the path, one that makes
  a whole segment `.`, `..` or empty makes the request `HTTP_INVALID` before anything is sent. The URL filled in is
  then checked as every URL is (the policy's patterns, its 8192 bytes, the rest). The host's list
  (`Program::webRequests()`) has a `*` for each hole, as for each `%d`. Each mistake is an error at its byte: a brace
  alone, an empty hole, a name that is none, a hole of another type ("hole {x} has type 'float': a hole takes an int, a
  bool, an enumeration's value or a string"), one in the host, a string's in the port, a `%d` beside a hole.
- **A request is an opaque `http_response`**, as an FFT plan is an `fft_plan`: C's handle, a pointer to a struct the
  program never sees, which the compiler keeps apart from the other handles (`http_status(doc)` of a `json_doc` is an
  error), while a `void*` converts to it and back, as in C, so a program that says `void*` compiles as before
  ([extending.md](extending.md), recipe 44). `defer http_close(r);` closes it on every path out of its block. A
  refused request is a request too, its status already set:
  `http_get` returns null only when memory ran out. A null handle does nothing and reads as a failed request:
  `http_status` and `http_wait` give `HTTP_INVALID`, `http_size` 0, `http_body` null, and `http_cancel` and
  `http_close` nothing.
- **The effects** ([extending.md](extending.md), recipe 11): `http_get`, `http_post`, `http_cancel` and `http_close`
  allocate or wait for the library's I/O thread, and are refused in a `[[clang::nonallocating]]` function and in a
  `[[clang::nonblocking]]` one; `http_wait` blocks (a lock and a condition variable) and is refused in a nonblocking
  function, allowed in a nonallocating one; the reads are wait-free, atomic loads of what the I/O thread published,
  and allowed in both. So a nonblocking `process` may poll for a late table and swap to it, and nothing on the audio
  thread can start, wait for or free a request.
- **The reads change from another thread.** Their declarations say so to LLVM (they are not `readonly`), so a polling
  loop reads the status again each time round, and two calls are never merged.
- **A body is bytes.** `json_parse`, `xml_parse`, `bytes_floats` and `bytes_pcm16` turn it into numbers, and
  `string_of(http_body(r), http_size(r))` views it as text, a `string` valid until `http_close`.

## The status codes

`http_status` and `http_wait` give 0 while a request is pending, the HTTP status (100 to 599) once a response has
arrived, whatever it is, or one of these when it ended without one. They are names a program uses without declaring
them, `int` constants, as an enumeration's constants are: they work in a `case` label and in a constant expression,
and a program's own declaration of such a name hides it, as a block's declaration hides the file's.

| Name | Value | The request ended |
|---|---|---|
| `HTTP_PENDING` | 0 | not yet |
| `HTTP_TRANSPORT` | -1 | with no response: the name did not resolve, the connection failed or was dropped, the response was no HTTP or was cut short |
| `HTTP_TIMEOUT` | -2 | past the policy's time, its redirects included |
| `HTTP_CANCELLED` | -3 | by `http_cancel`, `http_close`, or the host stopping or destroying the program |
| `HTTP_REFUSED` | -4 | refused by the host's policy: its URL, a redirect's, the address a connection reached, a content type; or the program has no network (the default) |
| `HTTP_LIMIT` | -5 | over a limit: the body (stopped at the limit, never downloaded whole), the redirects, the requests in flight, a POST's body, a URL over 8192 bytes |
| `HTTP_CERTIFICATE` | -6 | the server's certificate was not trusted or not for the host, or no root certificates were found |
| `HTTP_INVALID` | -7 | no request: a null handle, a URL libcurl cannot parse |
| `HTTP_FAILED` | -8 | the library itself failed: out of memory |

The server's builtins give the same names their own meanings ([below](#the-servers-builtins)): `http_port` is
`HTTP_REFUSED` when the host allowed no listen, `HTTP_TRANSPORT` when the address could not be bound and
`HTTP_CANCELLED` once the server is closed; a reply is `HTTP_LIMIT`, `HTTP_INVALID` or `HTTP_CANCELLED` when it was
not sent.

*Why names a program uses without a declaration, and not an `enum` it declares, nor macros:* FMinus never substitutes
a macro in code, so C's way for a library's constants (`EOF`, `errno`'s) is not open, and a builtin enumeration would
give the names a type of FMinus's own that a program cannot name. Constants with an enumeration's behaviour, outside
the file's scope, are what C's `<errno.h>` names would be if they were declarations.

## within

`within (milliseconds) { ... }` is a deadline around everything a block starts, and a promise about it: where the block
ends, every request the thread running it started in it has ended. It is `parallel { }`'s promise ("has finished,
every statement, where it ends") for requests, and Trio's `move_on_after`, Kotlin's `withTimeout`, with C's syntax and
no closure: the deadline is a scope, so nothing has to be passed down to the functions the block calls.

```c
enum { MOST = 16 };

float main(float count)
{
    const int packs = (int)count < MOST ? (int)count : MOST;
    http_response r[MOST];
    within (10000)                                   // all of them at once; none is still running after the block
    {
        for (int k = 0; k < packs; ++k)
            r[k] = http_get("https://example.com/packs/%d.json", k);
    }
    int loaded = 0;
    for (int k = 0; k < packs; ++k)                  // every r[k] has its final status: 200, a failure, HTTP_TIMEOUT
    {
        defer http_close(r[k]);
        loaded += http_status(r[k]) == 200 ? 1 : 0;
    }
    return (float)loaded;
}
```

- **The form**: the keyword, an `int` in parentheses, computed once where the block begins (a `float` converts as an
  argument does), and a block, braces required. A negative value is no deadline of its own: the block still waits for
  what it started, as long as each request's own policy lets it run.
- **The promise.** Every request the block started (`http_get`, `http_post`, a URL with holes as one without), in the
  block itself or in any function it called, has ended where it ends: the end waits for those still pending until the
  deadline, then cancels what is left, whose status is `HTTP_TIMEOUT` (time ended it, not the program). A request the
  block's code closed already is not the end's concern. Each request keeps its policy's own limits (`timeoutMs`,
  `maxRequests`) besides.
- **The deadline bounds every wait inside the block**: `http_wait`, `http_accept`, a `serve` with a timeout, and the
  block's own end. A wait whose own timeout is later than the deadline, or that has none (`http_wait(r, -1)`), returns
  when the deadline passes, with what it would have returned on its own timeout (`http_wait`: the status, 0 if still
  pending; `http_accept`: null; a `serve`: no request). A shorter timeout of its own still counts.
- **Leaving early.** `return`, `break` and `continue` out of the block, and a `reply` in a route, do what its end does
  first, exactly once: wait for or cancel what the block started, then leave. A `defer` in the block runs before that
  wait, as it does before the block's end; a `reply` is sent before it, so the route's client has its answer while its
  scope's requests end, as a route's deferred statements run after its reply (recipe 45). A `return`'s value is
  computed before either. The lowering is `defer`'s: the scope's end is a deferred action of a block around the
  braces, whose code is at each way out.
- **Nesting.** An inner block cannot outlast an outer one: its deadline is the earlier of its own and the outer's. A
  request belongs to the innermost block it was started in, and an outer block's end does not wait for an inner's
  (whose end has ended them already).
- **Whose requests.** A block's scope is its thread's and its program's: requests the thread running it starts, and the
  waits that thread makes. Two threads in two blocks do not see each other's. **A `parallel` region inside a block runs
  on other threads**: a request an iteration or a statement starts on one of the pool's threads is not the block's, the
  block's end does not wait for it, and its waits are not bounded by the block's deadline. (The calling thread works
  in the region too, and what it runs there is the block's: so which of a region's requests the end waits for is the
  pool's choice. Start a block's requests outside a region, or give the region's body a `within` of its own.) A
  `within` inside a region's body is the body's thread's own, and ends there (a region's body cannot be left any other
  way).
- **It blocks**, and its beginning allocates (the scope is kept in the program's context): a
  `[[clang::nonblocking]]` function refuses it in the words it refuses `http_wait` in, "must not call
  non-'nonblocking' function 'http_wait'", and a `[[clang::nonallocating]]` one in clang's words for an allocation,
  "must not allocate or deallocate memory", each at the `within`; a function that calls one holding it is refused at
  the call, with a note at the `within`. On the audio thread, where a program cannot wait, `when` is the statement:
  `when (http_status(request) == 200) ...` runs once, on the block the response arrived in (`examples/fetch/`'s
  `Live`).
- **A switch may not jump into one**: a `case` label inside its block, past where its scope begins, is clang's "cannot
  jump from switch statement to this case label", with a note at the `within` ("jump enters a 'within' block").
- **When its beginning fails** (no memory for the scope), the block runs with no deadline and nothing to wait for at its
  end, and its end ends nothing: an enclosing block's scope is not ended in its place.
- **A call the loop budget or a bounds check stops** (recipes 29 and 30) leaves the block without reaching its end, as
  it leaves every block, and runs no deferred statement. A scope is its thread's, found by the thread's id, so one left
  open would bound the thread's later waits by a deadline long past, and a later thread given the same id would find it.
  So each way out those checks make, in a function that begins a scope, first **abandons** the thread's scopes: no
  wait, nothing cancelled; their requests go on as requests started outside any block do, under the policy's limits,
  freed with the program if it never closes them; afterwards the thread has no scope, and its next block has its own
  deadline. The abandon takes no lock, allocates nothing and makes no system call (an audio thread's stop path may run
  it). A program compiled without either check is as it was.
- **In a build without the library** it is parsed, and is the error `http_wait` is there: "'within' needs the web
  library, which this build of FMinus leaves out (FMINUS_WEB=OFF)".
- **Underneath** are three of the library's functions (`src/web/Web.h`, "deadline scopes"): `within_begin` and
  `within_end`, which the block calls where it begins and at each way out, and `within_abandon`, which the stopped
  call's way out calls. `within` is a keyword; `await` is not (FMinus has no `async`, and a word reserved for nothing
  would break programs for nothing). [extending.md](extending.md), recipe 49, walks through it.

## The host's side

`Program::Options::web` is the policy (`include/fminus/FMinus.h`, `WebPolicy`):

| Member | Default | |
|---|---|---|
| `allow` | none | the URLs the program may reach, as patterns (below); none: no network |
| `maxBodyBytes` | 16 MiB | a response's body at most (and a POST's): past it, `HTTP_LIMIT`, stopped as it arrives |
| `timeoutMs` | 30 s | a request, its redirects included: past it, `HTTP_TIMEOUT` |
| `maxRedirects` | 5 | redirects a request follows, each checked again; one more is `HTTP_LIMIT` (0: none followed) |
| `maxRequests` | 16 | requests the program has in flight at once; one more is `HTTP_LIMIT` |
| `caFile` | empty | PEM roots to trust instead of the system's |
| `privateAddresses` | false | whether a pattern by name may reach a loopback, private or link-local address |
| `resolve` | none | names the host resolves itself, `"name:port:address"`, for this program alone: split DNS, tests |

`compile()` checks the policy: a bad pattern fails it, `fminus::Program: web: pattern 'gopher://x/' has the scheme
'gopher': only https and http are allowed`. In a build without the library the policy is not read.

**The permission model**, in the order a request meets it:

1. **The build** says whether FMinus can reach the network at all (`FMINUS_WEB`).
2. **The host** says, each time it compiles a program, what that program may reach. The default is nothing.
3. **Before anything runs**, `webRequests()` lists every call of `http_get` and `http_post` in the program: its method,
   its URL as a pattern (each `%d` a `*`, each `%%` a `%`), its file, line and column. URLs are literals, so the list
   is complete: a host can show it to its user and ask, or refuse to run a program that names a host it does not
   allow.
4. **When a request starts**, its URL (its `%d`s filled in) is parsed by libcurl, and refused unless a pattern allows
   it. The URL rules beyond the patterns: no control byte, space, backslash or non-ASCII byte; no userinfo of any
   kind, even empty; no IPv6 zone; no scheme but `http` and `https`; no percent-encoded `.`, `/` or `\` in the path
   (`%2e`, `%2f`, `%5c`, which a server may decode into a path the pattern did not mean), and no `.` or `..` segment
   with a parameter (`..;` or `.;x=1`, which libcurl keeps and a servlet container resolves); a host that is a valid
   name or an IP address. libcurl removes `.` and `..` segments before the match, and the request is made from the very
   handle that was checked, so the check and the request never disagree about what a URL means.
5. **At each redirect**, which the library follows itself, one hop at a time (libcurl never follows one), the target
   is checked again: a redirect to `http://169.254.169.254/` or to `http://localhost:…` is refused unless a pattern
   allows it. A 301, 302 or 303 after a POST is a GET without the body; 307 and 308 send it again.
6. **On each connection**, after the name is resolved, its address is checked against the pattern that allowed the
   URL: a name (or `*.name`) reaches public addresses only, never loopback, private (RFC 1918, `fc00::/7`),
   link-local, carrier-grade NAT, documentation, multicast or reserved ones (an IPv4 address inside an IPv6 one is
   judged as itself), unless `privateAddresses` is set; `localhost` reaches loopback only; an IP pattern exactly its
   address. So a name the policy allows cannot be pointed at the machine or its network, by DNS or by rebinding.
7. **Certificates are always verified** (TLS 1.2 or later, the peer and the host name), and the limits always apply.

Nothing a program asks of the library can widen any of it: the policy is the host's, the context that holds it is a
global of the program's module the program cannot name, and the library's own functions are names no function of a
program can take (any name that begins `fminus_web_`, a prototype's or a definition's, is an error in every build, so
a program cannot call the library past its literals and `webRequests()`; and the JIT does not have an executable's
`fminus_web_main_start`, which reads the environment).

**The policy is the library's, not a sandbox.** FMinus is C: a prototype with no definition declares an external
function, which the JIT resolves in the process, so a program can declare the C library's own `socket` and `connect`
and call them (tried here: `int socket(int, int, int);` compiles, and the call opens one), and a program has C's
pointers. A host has no way yet to refuse a program's external functions. So the policy holds a program that uses
the web library to what its host allowed, and `webRequests()` tells the host what such a program will ask for; neither
makes it safe to run a program written to get round them. That is FMinus's rule today for every external function,
not the web library's, and closing it is a change to the language's host interface, not to this library.

**The context.** A program that calls `http_get`, `http_post` or `http_listen` has a web context: a global of its
module, `fminus_web_context`, which the host's `Program` sets once when it compiles (as it sets the loop budget's), and
which every request passes to the library. A program given no patterns (and no listen) has none: the global stays null,
and the library refuses every request with no context.

**Limits are refusals the program reads**, never traps: `http_status` says `HTTP_LIMIT` or `HTTP_TIMEOUT`, and the
program decides what to do.

## The pattern language

Each pattern a string; a URL is allowed when one pattern matches it.

```
pattern = scheme "://" host [ ":" port ] [ path ]
scheme  = "https" | "http"        http is reached only by a pattern that says http (case is ignored)
host    = name                    that name exactly: example.com (one trailing dot is ignored, here and in a URL)
        | "*." name               any name ending in "." name, at least one label before it: not name itself
        | IPv4                    dotted decimal, no leading zeros; a URL may spell it as libcurl reads it
                                  (127.1, 0x7f.0.0.1, 2130706433): the address is what is compared
        | "[" IPv6 "]"            any spelling inet_pton takes, no zone
        | "localhost"             reached on a loopback address only
port    = 1 to 65535 | "*"        that port, or any; without one, the scheme's own (443, 80) only
path    = "/" chars [ "*" ]       the URL's path (never its query) equals chars, or with a final "*" starts with
                                  them: /presets/* matches /presets/a.json and /presets/a/b, not /presets or
                                  /presetsX; no path: any path; "/" alone is the path "/" ("/*" is any)
```

A `*` anywhere else, and `%`, `?`, `#`, `\` or `@` anywhere, are errors, as are spaces, controls and non-ASCII bytes,
and a name whose last label is all digits (or starts with `0x`) that is not an IPv4 address in dotted decimal.

| Pattern | Allows | Does not allow |
|---|---|---|
| `https://example.com/presets/*` | `https://example.com/presets/7.json`, `…/presets/a/b?x=1` | `http://example.com/presets/7.json`, `https://example.com:8443/presets/7.json`, `https://example.com.evil.net/presets/1`, `https://example.com/presets` |
| `https://*.example.com` | `https://cdn.example.com/a`, `https://a.b.example.com/` | `https://example.com/a`, `https://badexample.com/` |
| `http://127.0.0.1:*/packs/*` | `http://127.0.0.1:8000/packs/1.json`, `http://127.1:8000/packs/1.json` | `http://localhost:8000/packs/1.json`, `http://127.0.0.2:8000/packs/1.json` |

## The driver and executables

Neither has a host, so the person who runs them says what the program may reach, and by default it reaches nothing.

- **`fminus --run`** takes `--web-allow PATTERN` (repeated for more) and `--web-ca-file FILE`, which make the policy of
  the call's context; the other limits are the defaults. `--web-allow` with `--emit` is an error: an executable reads
  its own at run time. In a build without the library, `--web-allow` says so.

  ```bash
  build-web/bin/fminus load.fm --run load 3 --web-allow 'https://example.com/presets/*'
  ```

- **An executable** (`--emit exe`) of a program that calls the library reads its environment when it starts:
  `FMINUS_WEB_ALLOW`, the patterns separated by spaces, commas or newlines (unset or empty: no network), and
  `FMINUS_WEB_CA_FILE`. A pattern the library refuses stops it before `main` runs, with the reason on stderr and exit
  status 2, as for wrong arguments. Its context is destroyed when `main` returns.

  ```bash
  build-web/bin/fminus load.fm --emit exe -o load
  FMINUS_WEB_ALLOW='https://example.com/presets/*' ./load 3
  ```

  Such an executable links, after `libfminus_runtime.a`, `libfminus_web.a`, libcurl's and Mbed TLS's archives and the
  system libraries they need (pthreads on Linux; SystemConfiguration, CoreFoundation, CoreServices and Security on
  macOS; `ws2_32`, `iphlpapi`, `bcrypt`, `advapi32`, `crypt32` and `secur32` on Windows); one that does not call it
  links none of that. In the JIT, `fminus::defineRuntimeSymbols` defines the library's functions by address, from
  `src/web/WebSymbols.def`.

*Why an option and an environment variable:* a command line is what a person running the driver controls, and an
executable's command line is its `main`'s numbers (it has no option parser of its own), so its environment is what
its user controls at run time without rebuilding it. Neither is anything the program can write.

## Threads and lifetime

- **One I/O thread for the process** drives every request (libcurl's multi interface), and every server's sockets,
  started by the first request or listen and joined when the last program with a context goes. It is never one of the
  parallel regions' threads, which run at user-interactive priority and must not sit in a socket.
- **Each program has its own context**: its policy, its requests, and its own DNS entries and connections, never
  shared with another program's.
- **A request's result is published with one atomic store**, of its status, after its body and size. From then on the
  reads are wait-free, the body never moves until `http_close`, and the I/O thread never touches the request again.
- **`Program::stopWeb()`** stops the program's network for good: every request in flight ends as `HTTP_CANCELLED`
  before it returns, waking every thread in `http_wait`, and every request started from then on is cancelled at once.
  What completed stays readable. A host that shuts a program down while a thread of its own is inside it (a loader
  blocked in `http_wait`) calls `stopWeb()`, joins that thread, then destroys the `Program`. Destroying a `Program`
  while a thread still runs its code is undefined, as it always is.
- **A thread at a `within` block's end** is woken by `stopWeb()` as one in `http_wait` is: the block's requests end
  as `HTTP_CANCELLED`, and the block ends at once. A `within` begun after the stop has its requests cancelled as they
  start, and nothing to wait for.
- **`~Program`** stops the network, then destroys the context: every request the program made, closed or not, is
  freed, every thread still in the library's `http_wait` is woken and has left it, and the I/O thread is joined if this
  was the last context. A program that never closes what it opens leaks nothing past its `Program`, and no I/O thread
  is left running after the last `Program` of a plugin that is about to be unloaded.
- **That join can take as long as a DNS lookup.** A name is resolved on a thread of libcurl's, inside a call to the
  system's resolver that nothing can cancel; joining the I/O thread waits for it. So destroying the last `Program`
  while one of its requests is still resolving a name can take up to the system resolver's timeout (6 seconds,
  measured here, with a resolver that never answers and `options timeout:2 attempts:3`; glibc's defaults are 5 s and
  2 attempts), and a program elsewhere starting its first request waits for it too. A host that destroys programs on
  its UI thread does it on another thread, or calls `stopWeb()` early and destroys the `Program` later.

## Determinism and offline rendering

circuits.md's promise, that "the output never depends on how a host splits its blocks", cannot hold for a program
that starts using a table on the block in which it arrived: the block a response arrives in depends on the network.
**A program that polls on the audio thread gives up determinism**, and the manual says so in those words.

The load-then-run shape keeps it: fetch and parse in `init` or a loader method, off the audio thread, wait for the
result, and only then play. Then the output depends on the payload alone, as it does on a preset file.

A `within` block changes nothing of this. It waits as `http_wait` does, off the audio thread, and a request its
deadline cancels is one whose result depended on the network's timing, as a wait with a timeout's does: a loader that
must be repeatable gives its block a deadline it is content to fail at, and treats `HTTP_TIMEOUT` as it treats any
failure. A `when` that swaps a table in on the block it arrived in (`examples/fetch/`'s `Live`) is the polling shape
above, and gives determinism up in just those words.

An offline bounce should not depend on a server. A host that renders offline fetches before it renders (and refuses
to start, or reports, when a fetch failed); a disk cache and an `offline` policy (serve the cache or fail) are the
report's answer, and are not built yet (below).

## Plugins and entitlements

- **A sandboxed host** (an AUv3, an app from the Mac App Store) needs the network client entitlement
  (`com.apple.security.network.client`), which is the host's to have, not FMinus's: a plugin runs in its host's
  process with its host's entitlements. Without it, a connection cannot be made, which the program reads as
  `HTTP_TRANSPORT` (from what the sandbox is documented to do: not tried).
- **No listening socket** is ever opened by the client. The server opens one only where its host's policy says
  ([the server](#what-the-operating-system-does) has what the systems' firewalls and sandboxes make of it).
- **Symbols.** libcurl's and Mbed TLS's are hidden, and with the server llhttp's and fast_float's too; a plugin that
  exports only its entry points exports none of them, and they cannot clash with another copy in the host process.
  (On Windows llhttp's header marks its functions `__declspec(dllexport)`, so a DLL that links the server would export
  them: not built there yet.)
- **Unloading.** The I/O thread is joined when the last `Program` with a context is destroyed: destroy them before the
  plugin's code is unloaded. That can wait for a DNS lookup in flight, up to the system resolver's timeout (above).
- **Root certificates.** Windows: the system's store, through Schannel (revocation checked best-effort, so an
  unreachable revocation server does not fail requests). macOS: the system's anchors and the system and admin
  trust-settings domains, never the user's, read through Security.framework: a certificate of those domains only when
  its settings make it a root for TLS to any host (not one trusted only for S/MIME, code signing, one host name or one
  application), and never one either domain denies, a built-in anchor included; Apple's own partial or date-based
  distrust, which trustd applies, is not. Linux: the first of the usual bundle
  paths that exists (Debian's, Fedora's, openSUSE's, Alpine's ...); `SSL_CERT_FILE` is not read. A host with roots of
  its own names a file (`caFile`).
- **Proxies** are compiled out: `http_proxy` and `HTTPS_PROXY` in the environment are ignored.

## The server

A program serves when its host lets it: `http_listen()` starts its server where the host's policy says, and the program
**takes** the requests that reach it and answers each, as it polls a download's status. FMinus has no callbacks: a
program sees a request as a route it named, its parameters (numbers, and `string`s of its path and query), its bytes
and its text as views of what the library holds (`http_path`, `http_header`, `http_query_string`), valid until its
reply. Taking, reading
and answering a request are wait-free, so the audio thread may serve between two blocks, and a change lands at a block
boundary on the thread that computes the block, with no lock and no second thread touching the program's state. A
`serve` statement says it, FMinus's own ([its section](#serve-route-and-reply)):

```c
module Synth
{
    http_server server;
    float gain;
    float level;                                          // a meter process() keeps

    void init() { server = http_listen(); }               // where the host's policy says: nowhere by default

    void process(float* out, int n) [[clang::nonblocking]]
    {
        serve (server)                                    // wait-free: each request waiting for its routes, or none
        {
            route "POST /gain" (float value [[fminus::range(0, 1)]])   // POST /gain?value=0.5; out of range: 400
            {
                gain = value;
                reply 204;                                // answers, and leaves the route
            }
            route "GET /state"
            {
                reply 200, json { gain, level };          // {"gain":0.5,"level":0.25}
            }
        }
        // ... the block, with gain as it now is
    }
};
```

It is made of builtins a program can call itself ([the server's builtins](#the-servers-builtins)), the same requests
taken and answered: the loop above is `for (http_request r = http_next(server); r; r = http_next(server))` with a chain
of `if (http_is(r, "POST /gain"))`, the checks and the replies written by hand.

```cpp
fminus::Program::Options options;
options.web.server.listen = "localhost:0";                      // nothing given: nowhere
options.web.server.files = {{"/", "text/html; charset=utf-8", page}};   // the page, served by the library
auto program = fminus::Program::compile(source, errors, options);
for (const auto& route : program->webRoutes())                  // every route, before anything runs
    show(route.method, route.pattern, route.line);              // POST /gain 15, its parameter value, 0 to 1
// ... init() listens; then:
open("http://localhost:" + std::to_string(program->webPort()) + "/");
```

[examples/remote/](../examples/remote/README.md) is a whole one: a synthesiser, its page, and a harness that proves a
change sent over HTTP is heard in its output.

**What it is for**, and what it is not. It is a **control surface**, the thing a MIDI controller or a plugin's editor
is, reached from a browser or `curl`: a page with knobs and meters on the same machine or a phone on the LAN, a script
that sets a parameter or reads a level, a preset or a table pushed into a running program, a headless instrument
whose only interface is the network. It is not a web site, nor a service for strangers. Do not use it:

- **for anything public**, or anything that needs TLS, authentication of strangers, streams pushed from the server,
  or other origins' pages calling it: none of that is built ([what is not built](#what-is-not-built)), and the limits
  are a control surface's (sixteen connections, sixteen requests waiting);
- **for a host's own controls**: a host that wants a page for its parameters can serve one with any server it likes
  and call `Program::setValue`, which is already atomic; the program's server is for what only the program has (a
  meter it keeps, a table it reads, a route with a number in it);
- **for an offline render**: a bounce has no one to ask ([determinism](#the-servers-threads-and-lifetime)).

### Building the server

`FMINUS_WEB_SERVER=ON`, with `FMINUS_WEB=ON` (configure stops otherwise: "FMinus: FMINUS_WEB_SERVER is ON and
FMINUS_WEB is OFF: the server is part of the web library. ..."); the `web` preset sets both.

```bash
cmake --workflow --preset web                                  # the client and the server
cmake --preset release -DFMINUS_WEB=ON -DFMINUS_WEB_SERVER=ON   # any other tree
```

A first configure with it on fetches **llhttp 9.4.3**, Node.js's HTTP parser, into the deps cache
(`cmake/FMinusLlhttp.cmake`, pinned in `cmake/FMinusPins.cmake`: GitHub's archive of its `release/v9.4.3` tag, 40,403
bytes), and compiles its three generated C files into `fminus_web`, with their own flags and hidden symbols, never its
own CMake; and **fast_float**, for a query's numbers, if neither parser has fetched it already (`FMINUS_JSON` and
`FMINUS_XML` are on by default, and do). Nothing else: the server's sockets and every check are FMinus's own. llhttp is
MIT-licensed; a binary that includes the server carries its notice (the README's "License").

With the option off, nothing more is fetched or built, the server's sources under `src/web/` compile to nothing,
`fminus/Config.h` says `#define FMINUS_HAS_WEB_SERVER 0`, `__FMINUS_WEB_SERVER__` is not defined, each of the server's
builtins is a known name whose call is the error `'http_listen' needs the web server, which this build of FMinus leaves
out (FMINUS_WEB_SERVER=OFF)`, `Options::web.server` is not read, and the driver's `--web-listen` (and the options that
go with it) says the build leaves the server out. `http_size`, `http_body` and `http_close` are the client's, and stay
with `FMINUS_WEB`.

```c
#ifdef __FMINUS_WEB_SERVER__
    server = http_listen();
#endif
```

### serve, route and reply

Three statements of FMinus's own, as `parallel` is: not C's, and a program that uses them is no C program. They put
syntax over the builtins below, and make two of the server's rules ones the compiler checks: a request taken is
answered, once; and a route's numbers are in range before its code runs.

```c
serve (server)                 // each request now waiting for one of these routes, oldest first; wait-free
{
    route "POST /gain" (float value [[fminus::range(0, 1)]])          // POST /gain?value=0.5
    {
        gain = value;
        reply 204;
    }
    route "POST /note/{note}" (int note [[fminus::range(0, 127)]])    // POST /note/69
    {
        pitch = 440.0f * expf(0.05776227f * (float)(note - 69));
        reply 204;
    }
    route "GET /state"
    {
        reply 200, json { gain, pitch };
    }
}

serve (server, 100)            // the same, waiting up to 100 ms when none is waiting; blocks
{
    route "PUT /preset/{k}" (int k, http_request r) { ... reply 201; }
}
```

**`serve (server) { routes }`** takes each request now waiting **for one of its routes**, oldest first, and runs that
route's block; when none is waiting it is done. It is wait-free, as the loop over `http_next` is, and allowed in a
`[[clang::nonblocking]]` function when its routes' blocks are. **`serve (server, timeout_ms) { routes }`** first waits
up to `timeout_ms` for one (0: not at all; negative: until one comes or the server closes), as `http_accept` does, then
handles every one waiting and is done: it blocks, and a nonblocking function refuses it as it refuses `http_accept`
("function with 'nonblocking' attribute must not call non-'nonblocking' function 'http_accept'", at the `serve`).
`server` is an `http_server` (or a `void*`), the timeout an `int`, each computed once.

- **A serve takes only its own routes' requests.** A request for a route another `serve` of the program names, or an
  `http_is` elsewhere, is left waiting for that one, so a program answers `POST /gain` on its audio thread and
  `PUT /preset/{k}`, which parses JSON and so allocates, on a thread of its host's: two serves, two threads, each
  taking its own. (`http_next` and `http_accept` still take any request.) A request no one takes is answered 503 by
  the library at `replyTimeoutMs`, as one always was.
- **Its routes' blocks are blocks of the function the serve is in**: they see its locals, and in a method its module's
  fields; the function's effects are checked over them as over any block; no function is made, and nothing is called
  back. Only routes stand in a serve's braces, and a serve has at least one.

**`route "METHOD /path" (parameters) { block }`**, or with no parameter list. Its literal is a route as `http_is`'s is
([routes and replies](#routes-and-replies): the methods, the path's bytes, every error at its byte), with **holes**,
`{name}`, where a `%d` could stand: a whole segment or part of one (`"/osc/{k}/gain"`, `"/v{major}"`), at most 8 a
route, never two together nor one before a digit. `%d` is `http_is`'s spelling, and an error in a route. Each hole is
one of the route's parameters, each a different one. **A parameter** is `type name attributes`, the type `int`,
`float`, `bool`, `string` or `http_request` (`char`, and any other type, is refused). **A `string`'s hole is a whole
segment** (`"/presets/{name}"`; `"/presets/{name}.json"` is an error, "a string's hole is a whole segment of the path"),
a `%s` to the library:

| A parameter | Is filled from | When the request does not fit |
|---|---|---|
| named in a hole, an `int` | that part of the path: digits, as `%d` takes them (no sign; 1 to 10 digits that fit an `int`) | the route does not match: the library's 404 |
| named in a hole, a `string` | that segment of the path, its bytes as the server holds them (letters, digits, `-`, `.`, `_`, `~`; never `.` or `..`), valid until the reply | (a path of other bytes is the library's 400 before any route) |
| any other `int`, `float` or `bool` | the query's field of its name, read as `http_query_int` and `http_query_float` read (a `bool` is `true`, `false`, `1` or `0`) | missing, or not a value of its type: **400**, and the block does not run. With `[[fminus::default(x)]]`, missing is `x` (a malformed one is still 400) |
| any other `string` | the query's field of its name, its value's bytes as sent (nothing is decoded, as nothing is for a number), valid until the reply; `?v=` and `?v` are present and empty | missing: **400**, or its `[[fminus::default("text")]]` |
| with `[[fminus::range(a, b)]]` (path or query) | | outside it: **400**, and the block does not run |
| an `http_request` (one at most) | the request itself, for `http_body`, `http_size` and the rest | |

The 400s are the compiled code's, before the block: wait-free, a reply with no body. A parameter's attributes are a
module parameter's, checked as there (a range's ends in order, a default inside its range; an `int`'s default an
integer, a `bool`'s 0 or 1, a `string`'s a string literal, and a `string` has no range); a hole's has no default, and
`json::name` and `json::skip`, a module's fields' (`json_read`'s), are unknown attributes there. **Parameters are
`const`** variables of the route's block, in
scope there only, as a function's are in its body: two routes of a serve may use the same names. Attributes after the
parameter list are refused (`'clang::nonblocking' attribute cannot be applied to a route: ...`): a route is a block of
its function, whose attributes cover it.

**`reply status;`**, or **`reply status, more...;`**, answers the route's request and leaves the route. Its forms are
the four reply builtins' arguments without the request, the fourth's a JSON literal ([below](#the-json-literal)):

| Written | Is | Content type |
|---|---|---|
| `reply 204;` | `http_reply(r, 204)` | none |
| `reply 200, data, bytes, "content/type";` | `http_reply_bytes(r, 200, data, bytes, "content/type")` | as given |
| `reply 200, "content/type", "format", values...;` | `http_reply_text(r, ...)`, a format as [below](#routes-and-replies) | as given |
| `reply 200, json { ... };` | `http_reply_json(r, 200, json { ... })`: the literal, written into the request's buffer | `application/json` |

Which form it is follows from the values (one; a string literal second, or a `string`, which is the text form's
error, its content type and format literals only; a JSON literal second; four), and each is checked as its builtin
checks its arguments, in the same words; a reply that fits none says what the forms are.

- **It leaves the route**, as `return` leaves a function: the reply is sent, then the deferred statements of the route's
  blocks run (as a `return`'s value is computed before them), and the serve goes on to its next request.
- **Every path through a route must reach a `reply`**, the analysis of a non-void function's missing `return`, constant
  conditions and all: `route 'GET /state' does not reply` (no path does) or `... does not reply in all control paths`,
  at the route's closing brace. A `when`, `when (c) reply 204;`, may not run its statement, as an `if` without an
  `else`; a `switch`, on a `string` too, replies on every path only with a `default`.
- **What a route may not do**, each an error at its keyword: `return` ("cannot return from a route: a route ends with a
  reply"); a `break` or `continue` that would leave it (one of a loop of its own is a loop's); a `serve` of its own; a
  `reply` in a deferred statement ("cannot reply from a defer statement"); and a `case` label a `switch` around the
  serve would jump to. A `reply` outside a route is "'reply' statement not in a route".
- **A reply the library refuses** with the request still the route's, `HTTP_LIMIT` (over `maxReplyBytes`) or
  `HTTP_INVALID` (a status outside 200 to 599), is answered 500 by the compiled code, so the request is consumed all the
  same; after `HTTP_CANCELLED` there is nothing to do.

So **a request a serve takes is answered exactly once, by construction**, unless the loop budget or a bounds check
stops the call it is in ([extending.md](extending.md), recipes 29 and 30): a call they stop returns at once, with no
reply and no deferred statement run, and its request holds its slot until the server closes, as any request taken and
not answered does.

**What the compiler knows.** Every route literal of the program, a route's (each hole a `%d`) and an `http_is`'s, is in
the route table `http_listen` hands the library, and in `Program::webRoutes()`, a route statement's with its
parameters: each one's name, type, what fills it (`"path"`, `"query"` or `"request"`), its range and its default (a
`string`'s in `defaultText`), so a
host can build a control page, or an OpenAPI description, from a program it has not run. And **two route literals one
request could match are an error** (at the second, with a note at the first), unless one is more specific, Go's
`ServeMux` rule (<https://pkg.go.dev/net/http#ServeMux>, "Precedence"): a literal segment is more specific than one with
a hole. The library hands a request to the most specific route that matches, ranking segment by segment from the
first: a literal one before one with fewer holes before one with more, and those before a `string`'s, which takes any
segment. So `GET /presets/default`, `GET /presets/{k}` (an `int`) and `GET /presets/{name}` (a `string`) may all be
routes, each request the first of them that fits; `GET /a/{s}` and `GET /{t}/b` (two strings) may not. So `GET /a/1` and `GET /a/{x}` may both be
routes (`/a/1` is the first's, `/a/2` the second's), as may `GET /b/{x}-1` and `GET /b/{x}-{y}`; `GET /a/{x}/y` and
`GET /a/1/{z}` may not (`GET /a/1/y` matches both, and neither is more specific), nor `GET /c/1{x}` and `GET /c/{x}`
(each a segment of one hole, which the library ranks the same), nor the same route in two serves (which would take
it?). The same literal in two `http_is` calls stays legal: a program may test a request twice. The check reads a hole
as any number of digits, so it refuses two routes that would differ only past ten digits.

**What it lowers to**: the builtins, called as a program would call them, and three of the library's own:
`fminus_web_http_next_in` (or, with a timeout, `fminus_web_http_accept_in`) of the serve's route set, one bit for each of
its routes in the table, a constant of the program; `fminus_web_http_route`, the route a request was published for,
which also makes that route's numbers the request's (for `http_arg`, and a `string`'s segment for
`fminus_web_http_text`); and `fminus_web_http_query_text`, whether the query has a field, and a `string`'s bytes. A loop over the takes, a `switch` on the route, each case its parameters filled and checked and its
block, each reply a jump to the next take ([extending.md](extending.md), recipe 45).

### The JSON literal

```c
reply 200, json { "gain": gain, "pitch": pitch, "changes": changes, "on": active };
reply 200, json { gain, pitch, level, changes };              // a name alone is its own key
reply 200, json { "state": { "gain": gain }, "taps": taps, "wave": wave };   // an array, an enumeration's name
reply 200, json (preset);                                     // one value: a module is an object of its fields
http_response sent = http_post("https://api.example.com/summaries", json { "days": day });   // an array of modules
file_write("presets/{name}.json", json (preset));             // a file's bytes ([files.md](files.md))
file_append("log/days.jsonl", json { "day": k, "mean": mean });   // and a newline after them: JSON Lines
```

**A JSON literal is an argument, never a value**, as a format is: `json { members }`, `json [ values ]` and `json
(value)` are expressions to the grammar, and the compiler takes one only where something writes it: a `reply` and
`http_reply_json`, the body of `http_post`, and the bytes of `file_write` and `file_append` ("FMinus has no JSON
values: a JSON literal is only the body of a reply, http_reply_json, http_post, file_write or file_append" anywhere
else). No JSON value is ever made: the compiler makes a constant description of the literal (its keys and structure as
bytes, and each value's shape: [extending.md](extending.md), recipe 51), the values are stored in slots of the
function's frame, once each, in the order written, and the runtime's JSON writer (`fminus_rt_json_write`, part of
`FMINUS_JSON`) writes the text where it goes. **It needs the JSON writer**: in a build without `FMINUS_JSON` a JSON
literal is the error that says so ("a JSON literal needs the JSON writer, which this build of FMinus leaves out
(FMINUS_JSON=OFF)"), and `json_slice` is the error that names the option.

| A value that is | Is written as |
|---|---|
| a `float` or a `double` | **the shortest digits that read back as the same value** (`std::to_chars` with no precision: `0.1f` is `0.1`, `1.0f / 3` is `0.33333334`, `1e20f` is `1e+20`); a NaN or an infinity, which JSON has no number for, `null`. The same in every locale |
| an `int`, a `bool` | in decimal; `true` or `false` |
| a `string` (a literal among them) | a JSON string, `"`, `\` and U+0000 to U+001F escaped as RFC 8259 asks (`\"`, `\\`, `\b`, `\f`, `\n`, `\r`, `\t`, `\u00XX`), every other byte as it is (no UTF-8 check), so a string from a request, a document or a listing cannot end the literal's string or add a member |
| **a value of an enumerated type** | **its constant's name**, a JSON string (`"SAW"`: the first constant of that value, as written); a value no constant has, its number. **This replaces the error of recipes 45 to 50** (which said to cast to `int`, and still does what it did: `(int)wave` is a number) |
| a `char` array | a JSON string of its bytes up to its first 0 (or all of them) |
| an array of one dimension, of anything below | a JSON array of all its elements |
| an array of two dimensions | an array of arrays, a row each |
| a `float2`, `float4`, `float8` | an array of its lanes |
| **a module**, a value or what a pointer points to, `*p` | an object of its fields, in their order (below) |
| an array of modules | an array of such objects |
| `json_slice(array, count)` | the array's first `count` elements, the count clamped to the array's size (`json_slice(day, n)`); or of a pointer, `json_slice(p, n)`, the program vouching for the `n` elements, as it does for `bytes_floats` |

- **A member** is `"key": value`, or a bare name, which is `"name": name`. A key is a string literal (adjacent ones are
  one), written as a JSON string, escaped as above, and UTF-8 text (a byte that is no part of any is an error). A
  duplicate key in one object is an error at the second. A value may also be a nested `{ }` or `[ ]`.
- **A module's object** is its fields in their order, each under its own name or its `[[json::name("...")]]` (two
  fields under one JSON name are an error), the names `json_read` reads them by ([README.md](../README.md), JSON): the same
  fields, under the same names, as `json_read`'s table. Left out: a field with `[[json::skip]]`, a pointer, a handle
  and a poly type (none has a JSON value), and a `when`'s hidden state. A `string` field **is** written (`json_read`
  leaves one out, since it would point into the document). A field that is a module or an array of them nests.
- **A module's parameter** is the host's value, which only a method of its module reads (as `cutoff[i]`). So a literal
  writes one only as **`*this`, in a method of its own module**, each parameter as `cutoff[0]` reads it there: a knob's
  value, a frozen one's constant, a stream's first sample of the call. A module with a parameter written anywhere else
  (another instance, a pointer's `*p` outside its methods, a module or an array holding one) is an error that says so,
  and that `[[json::skip]]` leaves the parameter out ("a JSON literal writes a parameter's value only where a method of
  its module reads it, as *this: 'cutoff' of 'Synth' is one (leave it out with [[json::skip]])").
- **Not a value**: a pointer ("write what it points to, *p, or its first elements, json_slice(p, count)"), a handle, a
  poly type, a bool vector, and an array of any of them, each an error saying what a value may be. `json_slice`
  anywhere but in a literal is an error, and its elements are never `char`s (a `char` array is a string, written
  whole).
- **`json (value)`** is a literal of one value at the top: `json (preset)` an object, `json (taps)` an array, `json
  (0.5f)` a number.
- **Nothing is built, and the shape is the program's**: an array's and a module's layout is known when the program is
  compiled, so the description says where each field is, and the writer walks it, nested 16 deep at most, a module of
  1024 fields at most, with no allocation, lock or system call (`tests/unit/JsonWriteTest.cpp`, and through the JIT
  `JsonServeTest.cpp`): a reply's literal of modules is wait-free on the audio thread.
- **Where it goes**: a **reply**'s literal, `reply status, json ...;` or **`http_reply_json(r, status, json ...)`**
  (the builtin a reply of a literal is made of, for programs in the `http_is` style, whose result is the library's:
  `0`, or `HTTP_LIMIT` for a text over the host's `maxReplyBytes`, which a route answers 500), is written straight into
  the request's own reply buffer (`fminus_web_http_reply_buffer`) and published (`fminus_web_http_reply_written`),
  wait-free. A **POST**'s, `http_post(url, json ..., ints...)`, is the request's body, sent as `application/json`: the
  library measures the text (a first pass of the writer, which writes nothing), refuses one over the policy's
  `maxBodyBytes` (`HTTP_LIMIT`, and nothing is sent), and writes it once into the request's own body, a buffer of
  exactly its size, before `http_post` returns (it allocates, as `http_post` always has). A **file**'s, `file_write` and
  `file_append`: [files.md](files.md), "Writing".
### The server's builtins

What a `serve` is made of, and what a program may still call itself: a request taken with `http_next` or `http_accept`
and told apart with `http_is` is answered by hand, as before the statements were.

| Builtin | Does | Effect |
|---|---|---|
| `http_server http_listen()` | starts the program's server where its host's policy says, with the program's routes, and returns it at once. A program listens once: a second call returns the same server | allocates |
| `int http_port(server)` | the port it listens on (1 to 65535), or why it does not: `HTTP_REFUSED` (its host allowed no listen: the default), `HTTP_TRANSPORT` (the address could not be bound: taken, not this machine's, or a sandbox forbids it), `HTTP_CANCELLED` (closed, or the host stopped the program), `HTTP_FAILED` | wait-free |
| `http_request http_next(server)` | the oldest request waiting, now the caller's to answer, or null | wait-free |
| `http_request http_accept(server, timeout_ms)` | the same, waiting up to `timeout_ms` for one (0: not at all; negative: until one comes or the server closes); null at once for a server that does not listen | blocks |
| `bool http_is(r, "METHOD /path/%d")` | whether the request is the route's: its method and its path, each `%d` a decimal number of the path; when it is, it keeps the numbers for `http_arg` | wait-free |
| `int http_arg(r, i)` | the `i`-th number (from 0) the last `http_is` that matched took, or 0 | wait-free |
| `float http_query_float(r, "name", fallback)`, `int http_query_int(r, "name", fallback)` | a number of the request's query (`?name=0.5`), or the fallback | wait-free |
| `string http_header(r, "name")` | the request's first header field of that name (a literal, an HTTP token: letters, digits and ``!#$%&'*+-.^_`\|~``, compared without regard to ASCII case), its value without the spaces and tabs around it; empty when there is none. With a token in the server's policy, `Authorization` is the host's and reads empty | wait-free |
| `string http_path(r)` | the request's path as sent, without its query (`/osc/3/detune`) | wait-free |
| `string http_query_string(r, "name", fallback)` | the query's first field of that name, its value's bytes as sent (nothing decoded), or the fallback when it has none (`?v=` is present and empty): what a route's `string` parameter of the query reads | wait-free |
| `int http_size(r)`, `const void* http_body(r)` | the request's body (the client's two builtins, on a request the program received), until it is answered | wait-free |
| `int http_reply(r, status)` | answers with no body | wait-free |
| `int http_reply_bytes(r, status, data, bytes, "content/type")` | answers with `bytes` bytes at `data` (copied: the buffer is free again at once) | wait-free |
| `int http_reply_text(r, status, "content/type", "format", ...)` | answers with text, C's `printf` of the values after the format (`%s` a `string`'s bytes) | wait-free |
| `int http_reply_json(r, status, json ...)` | answers with a JSON literal's text, written in the request's own buffer, as `application/json` ([The JSON literal](#the-json-literal)): what `reply status, json ...;` is | wait-free |
| `void http_close(server)` | stops listening and closes every connection: a request the program holds is given up, every thread in `http_accept` woken (null), the port `HTTP_CANCELLED`; the replies the program has answered are sent first, for a quarter of a second at most ([below](#the-servers-threads-and-lifetime)) | allocates |

- **A request is the caller's from `http_next` or `http_accept` until its reply**, and only the thread that took it
  should read or answer it (a program that hands it to another thread orders the two itself). One reply each: the
  handle is dead after it, as a client's request is after `http_close`. Until then its method, path, query, header
  fields and body never change, and **a `string` read from it (a parameter, `http_path`, `http_header`,
  `http_query_string`) is valid until its reply**, after which its bytes are the next request's: the compiler does not
  check it, as it does not check a body's pointer. Its text is bytes as sent: nothing is decoded, and no encoding is
  checked.
- **A reply** is a status of 200 to 599 (204 and 304 with no body) and returns 0 once it is the library's to send;
  or, having sent nothing, `HTTP_LIMIT` (the body is over the policy's `maxReplyBytes`: the request is still the
  caller's), `HTTP_INVALID` (not a request the caller holds, or a status, a size or a content type that cannot be:
  still the caller's), or `HTTP_CANCELLED` (the library gave up on it meanwhile: its client left, it timed out and was
  answered 503, or the server closed; the handle is dead). A reply that returned 0 is sent, a close right after it
  included. The content type is a literal of printable ASCII, at most 255
  bytes; empty means none for `http_reply`, `application/octet-stream` for bytes and `text/plain; charset=utf-8` for
  text. A `HEAD` request's reply keeps its `Content-Length` and drops its body.
- **A query's number** is the value of the first field of that name, compared byte for byte (nothing is decoded) and
  read whole, or the fallback when there is none or it is not all a number. A float is read by fast_float (decimal or
  exponent form, no `+`, no hex), correctly rounded, and must be finite: `nan`, `inf` and a number past a float's
  range give the fallback, so nothing a stranger types into a URL puts a NaN into a filter's state. An int is an
  optional `-` and digits whose value an `int` holds (`7.0` is not one). The range is the program's to check: a gain
  of 7 is a number.
- **Handles know what they are.** A request made, a request received and a server are an `http_response`, an
  `http_request` and an `http_server`, and the compiler refuses one where another is wanted (`http_close` takes a
  request made or a server, `http_size` and `http_body` a request made or received). One that reaches a builtin as a
  `void*`, which converts to any, is checked again in the library, as it always was: each carries its kind, and a
  builtin given the wrong one does nothing and says `HTTP_INVALID` (or 0, or null). `http_close` of a request the
  program received does nothing; `http_status` and `http_wait` of a server say `HTTP_INVALID`.
- **The effects** ([extending.md](extending.md), recipe 11): `http_listen` and `http_close` are refused in a
  `[[clang::nonallocating]]` function and in a `[[clang::nonblocking]]` one; `http_accept` blocks (a lock and a
  condition variable), refused in a nonblocking function and allowed in a nonallocating one; everything else is
  wait-free, allowed in both. So a nonblocking `process` may take and answer requests, and nothing on the audio thread
  can start, wait on or close a server.
- **The reads change from another thread**, and a reply hands the request back: their declarations say so to LLVM
  (`http_next`, `http_is` and the reads are not `readonly`; a reply may write any memory), so a polling loop calls
  `http_next` each time round, and a load of a request's body after its reply is not the one before it ([extending.md](
  extending.md), recipe 42).

### Routes and replies

**A route is a string literal**, a method, one space and a path, checked where it is written, each mistake an error
at its byte:

```
route  = method " " path
method = "GET" | "POST" | "PUT" | "PATCH" | "DELETE"      a GET route answers HEAD too
path   = "/" | ("/" segment)+                             no empty, "." or ".." segment, no trailing "/"
segment: letters, digits, "-", ".", "_", "~", and %d      a decimal number of the request's path, 1 to 10 digits that
                                                          fit an int, all the digits there are; at most 8 a route,
                                                          never two together nor one before a digit
```

| Literal | Error |
|---|---|
| `"get /x"` | `'get' is not a method a route can name: GET, POST, PUT, PATCH or DELETE` |
| `"HEAD /x"` | the same, with `(a GET route answers HEAD too)`; `OPTIONS`: `(the library answers OPTIONS itself)` |
| `"GET  /x"` | `a route has one space between its method and its path` |
| `"GET /a//b"`, `"GET /a/../b"` | `a route's path has no empty segment`; `a route's path has no '.' or '..' segment` |
| `"GET /a/%d%d"`, `"GET /%d5"` | `a %d right after another can never match: each takes all the digits there are`; `a digit right after %d can never match: %d takes all the digits there are` |
| `"GET /%d/.../%d"` (nine) | `a route takes at most 8 %d` |
| `"GET /a/%s"` | `invalid conversion specifier 's'`, with the note `a route takes %d, a decimal number of the path` |
| `"GET /a?x=1"` | `a route is a path alone: a request's query is read with http_query_float and http_query_int` |
| `"GET /a b"`, `"GET /100%%"` | `character ' ' is not allowed in a route's path: letters, digits, '/', '-', '.', '_', '~' and %d`; `a route's path cannot hold '%': %d, a number, is its only conversion` |

Every route literal in the program, in every function, goes into **the program's route table**, each text once,
which every `http_listen` hands the library: so **the library answers what no route names**, 404 for a path no route
has and 405 with `Allow` for a method none has for it, and only a request the program declared ever reaches its code.
The same routes are what `Program::webRoutes()` lists. A route literal's path is the request's whole path: a request's
query is never matched, and is read with the query's builtins. `%d` takes a number with no sign (`/osc/-1` matches
nothing); a program bounds it itself (`http_arg(r, 0) & 15`, or a 400).

**A reply's text is a format**, C's `printf`'s in part, checked as clang checks `printf`'s and in its words:

| Place | Takes | Writes |
|---|---|---|
| `%d` | an `int` or a `bool` | the value in decimal |
| `%g` | a `float` or a `double` (a float as `printf` promotes one) | six significant digits, as `printf` writes them |
| `%.Ng`, N from 1 to 17 | the same | N significant digits: `%.9g` reads back as the same float, `%.17g` as the same double |
| `%s` | a `string` (a literal among them) | its bytes as they are (nothing escaped: a JSON literal escapes) |
| `%%` | nothing | `%` |

Errors: `format specifies type 'int' but the argument has type 'float'`, `format specifies type 'double' but the
argument has type 'int'`, `format specifies type 'string' but the argument has type 'int'`, `more '%' conversions than
data arguments`, `data argument not used by format string`,
`invalid conversion specifier 'f'`, and FMinus's own `a reply's %.Ng takes a precision N of 1 to 17`. The text is
written by `std::to_chars` into the request's own buffer, the same in every locale, with no allocation. As in C, a NaN
is `nan` and an infinity `inf`, which JSON cannot hold: a program that might have one says so itself. Arrays of numbers
go as bytes (`http_reply_bytes(r, 200, table, 8192, "application/octet-stream")`, a `Float32Array` to a page), which is
also what `bytes_floats` reads on the way in (`bytes_floats(http_body(r), http_size(r), table, 2048)`).

**A query's name** is a literal of letters, digits, `_`, `-` and `.`, 1 to 64 bytes (`character ' ' is not allowed in a
query's name: ...`, `a query's name is 1 to 64 bytes, not 0`); a content type, as the client's (`character '\x0a' is
not allowed in a content type: it is printable ASCII`).

### How a request reaches the audio thread

The server keeps a fixed table of **slots**, `maxPending` of them (16 by default), each with a buffer of
`maxReplyBytes` for its reply (64 KiB), all made when the server starts. The I/O thread receives a whole request, head
and body, within the limits, fills a free slot and publishes it with one atomic store. Then:

| Step | Who | How |
|---|---|---|
| take | the program (`http_next`) | scans the table for the oldest published slot and claims it with one compare-and-swap: at most twice the slots' number of tries, no lock, safe from several threads |
| read | the program | the method, path, query and body never change while the slot is claimed: plain reads |
| answer | the program (`http_reply*`) | writes the status, the headers and the body into the slot's own buffer and publishes them with one compare-and-swap |
| send | the I/O thread | sees it on its next turn, sends, and frees the slot |

The I/O thread is not woken for a reply, since waking it is a system call and the audio thread makes none: while a slot
is published, claimed or answered it turns once a millisecond, and otherwise when a socket, a command or a deadline
wakes it. So a reply goes out about a millisecond after the block that wrote it, and a program whose server has
nothing waiting costs the I/O thread nothing.

**What it costs** (measured here on the server as built, Linux x86-64, an AVX-512 Xeon, with a scratch program over
`libfminus_web.a`, calling the C functions the builtins call; not part of the tree):

| What | Measured |
|---|---|
| `http_next` with nothing waiting, 16 slots (the default) | about 10 ns |
| the same, 256 slots | about 150 ns: the scan is linear in the slots |
| one request handled: taken, three routes tried, one `%d` and a query's float read, a reply of two numbers formatted | 0.65 to 0.95 µs, the best of 20 rounds of 16 or 256 requests (the slot's memory was just written by the I/O thread, so a request starts in cache misses) |
| a reply's store to its client's first byte, over loopback | 1.18 ms median, 1.24 ms in 90 % of 200, 1.34 ms at most (the 1 ms turn) |

The research's prototype measured 10 ns and 180 ns; the built server's handling is slower, the cold slot and a
real reply's headers being the difference. **The bounds:** a block that finds every slot waiting answers all of them, so
a `process` that serves can take `maxPending` requests in one block (16 by default: about 15 µs at the costs above);
`http_is` compares a literal with a path of at most the head's 16 KiB; a reply's text is at most `maxReplyBytes`. A
program that would rather spread them takes a few a block (`for (int i = 0; i < 4; ++i)`), and the rest wait for the
next.

**What it buys and costs.** A reply's body is bounded by its slot's buffer: a larger one is refused (`HTTP_LIMIT`), and
nothing on the audio thread ever allocates to answer. **A request taken must be answered**: a `serve` answers each of
its own exactly once, by construction (unless the loop budget or a bounds check stops the call it is in); one taken with
`http_next` or `http_accept` is the program's to answer, even if only with `http_reply(r, 404)`. At `replyTimeoutMs` the
library answers its client 503 for the program, and the program's reply, whenever it comes, returns `HTTP_CANCELLED`;
but the slot stays the program's until that reply, since the program may still be writing into its buffer. A request
never answered keeps its slot until the server closes, and `maxPending` of them leave every later request a 503
(`tests/unit/WebServerTest.cpp` pins this). A program that must parse what it is sent with `json_parse`, which
allocates, takes those requests on a thread of its host's with `http_accept` instead, the server's one builtin that
blocks.

That the wait-free builtins are wait-free in the library's code is proved, not asserted
(`tests/unit/WebServerTest.cpp`): `HeapCalls` counts no allocation in `http_next`, `http_port`, `http_is`, `http_arg`,
the query's reads, `http_size`, `http_body` and each reply, and a child process under seccomp's strict mode, in which
any system call but `read`, `write` and `exit` kills it, makes the same calls and sends back the expected results
(`tests/unit/StrictMode.h`, which every such proof uses; [testing.md](testing.md), "A wait-free proof").
`tests/unit/WebServeTest.cpp` makes the same proof of a `[[clang::nonblocking]]` handler compiled by the JIT, whose
builtins take, read and answer the request through the definitions the JIT gives them, and `tests/unit/ServeTest.cpp`
of a `serve` in a nonblocking method: its takes by route set, its parameters' reads and checks, each form of reply and
a JSON literal (Linux).

### What the host gives the server

`Program::Options::web.server` is the policy (`WebServerPolicy`, `include/fminus/FMinus.h`). Its default listens
nowhere.

| Member | Default | |
|---|---|---|
| `listen` | empty | where the program may listen (below); empty: nowhere, and `http_port` is `HTTP_REFUSED` |
| `hosts` | none | names a request's `Host` may say besides the address (`studio.local`), without a port |
| `token` | empty | a secret every request to a route must carry: 16 to 256 characters of `A`–`Z`, `a`–`z`, `0`–`9`, `-` and `_`, from the system's random source |
| `open` | false | a listen that is not loopback, with no token: the host means an open port |
| `files` | none | static content the library serves itself: a path (one a route could name, each once), a type, the bytes |
| `maxConnections` | 16 | connections open at once (1 to 1024); one more is closed at once, unanswered |
| `maxPending` | 16 | requests the program has not answered yet, the slots (1 to 1024) |
| `maxRequestBytes` | 1 MiB | a request's body (0 to 2^30): past it, 413 |
| `maxReplyBytes` | 64 KiB | a reply's body (0 to 2^24, and all slots' buffers at most 2^28 bytes): past it, `HTTP_LIMIT` |
| `requestTimeoutMs` | 10 s | to send a whole request, head and body (408 past it), and to take a reply |
| `replyTimeoutMs` | 2 s | for the program to answer one, before the library answers 503 for it |
| `idleTimeoutMs` | 15 s | a connection kept alive with nothing on it (each time 1 to 86,400,000 ms) |

| `listen` | Binds |
|---|---|
| `"127.0.0.1:8080"` (any of 127.0.0.0/8), `"[::1]:8080"` | that loopback address |
| `"localhost:8080"` | both loopback addresses, 127.0.0.1 and ::1, one port: a browser may try either first. On a machine without IPv6, 127.0.0.1 alone |
| `"localhost:0"`, `"127.0.0.1:0"` | a port the system picks (the same on both addresses): what a plugin with several instances wants. `webPort()` says which |
| `"192.168.1.20:8080"`, `"[fd00::1]:8080"` | one address of the machine: not loopback, so a token or `open` is needed |
| `"*:8080"` | every interface, `0.0.0.0` and `::`: likewise |

`compile()` checks the policy, as it checks the client's, and refuses a mistake with the first one it finds:
`fminus::Program: web: server.listen '0.0.0.0:8080': 0.0.0.0 is the unspecified address: write '*' for every
interface`. Refused besides: a port browsers refuse (the Fetch standard's "bad ports", 6000, 6665 to 6669, 10080 and
the others: no browser would open the page), multicast, broadcast and 240.0.0.0/4 addresses, 0.0.0.0/8, IPv6
link-local ones (which need a zone), an IPv4 address inside an IPv6 one, a name other than `localhost`, a missing port
or one with a leading zero, and a listen that is not loopback without `token` or `open` (`server.listen '*:8080' is not
a loopback address: the policy needs a token (server.token), or open (server.open) ...`). The server's part is checked
whether or not it listens; in a build without the server it is not read.

- **The address is the host's, never the program's.** `http_listen()` takes no argument: a program cannot choose to be
  reachable from the network, nor a port its host did not give it. It reads where it ended up (`http_port`), and so
  does the host.
- **`Program::webRoutes()`** lists every call of `http_is` in the program, before anything runs: its route's method,
  its path as a pattern (each `%d` a `*`), its file, line and column, in the order the calls were lowered, duplicates
  included. What the program will answer, as `webRequests()` is what it will ask.
- **`Program::webPort()`** is the port once the program has listened (1 to 65535), and 0 before, when its policy has
  no listen or the address could not be bound, once the server is closed, and in a build without the server. It takes
  a lock: not for the audio thread (the program's own `http_port` is wait-free).
- **Static content is the host's.** A page's HTML, script and style are not the program's business, and FMinus has no
  way to hold them. The host hands each over as a path, a type and its bytes, and the library serves it itself, on the
  I/O thread, without the program, to `GET` and `HEAD` (and with 405, `Allow: GET, HEAD`, to anything else). There is
  no mapping from a path to a file at request time, so nothing to traverse. `web::readServerRoot` (`src/web/Web.h`)
  makes the list from a directory for a host that has files rather than bytes: the driver's `--web-root` and an
  executable's `FMINUS_WEB_ROOT` use it (below).
- **A context exists with a listen alone**: a program may listen and reach nothing (`http_get` then says
  `HTTP_REFUSED`), as it may reach and not listen.
- **`Program::stopWeb()`** closes the server too: its sockets and every connection, every request the program has not
  answered given up (its reply `HTTP_CANCELLED`), every thread in `http_accept` woken (null), and `http_port`
  `HTTP_CANCELLED` from then on; an `http_listen` after it never listens. The replies the program answered before it
  are sent first, so it may take a quarter of a second more when a client does not read its reply (below). `~Program`
  stops, then frees it.

### What a request meets

Everything that can reach the port is hostile input, and on a desktop that is more than it seems: **a page in the
user's browser, from any site, can send requests to 127.0.0.1**, and by rebinding a name of its own to 127.0.0.1 it can
make them look same-origin to the browser. Local servers in shipping software have fallen to exactly this for years
(the research's §6 has the cases). The library checks every request, in this order, on the I/O thread, before the
program sees it; each refusal is a status with an empty body, and the connection is closed after it unless it says
otherwise:

1. **The connection.** Beyond `maxConnections`, it is accepted and closed at once, unanswered. Every socket is
   non-blocking, never inherited by a child process, and never raises `SIGPIPE` (on Linux `SOCK_CLOEXEC`, `accept4`
   and `MSG_NOSIGNAL`; `src/web/Sockets.h` has macOS's and Windows' ways, never built).
2. **The head.** At most 16 KiB and 64 header fields (431), within `requestTimeoutMs` of its first byte (408); a
   connection that sends no byte at all in that time is closed unanswered. HTTP/1.1 or 1.0 (0.9 and 2.0 are 505; a
   version llhttp does not parse, 1.2, is 400). A method of `GET`, `HEAD`, `POST`, `PUT`, `PATCH`, `DELETE` and
   `OPTIONS` (501). A target in origin form, whose path is `/` or segments of letters, digits, `-`, `.`, `_` and `~`,
   with no percent-encoding and no empty, `.` or `..` segment, so there is nothing to normalise and nothing two parsers
   could read differently, and whose query is printable ASCII without `#` (400). `Transfer-Encoding` is 501, so llhttp's
   chunked machinery is never entered; `Upgrade` 400; `Expect` 417; a body's length comes from `Content-Length` alone.
   Anything else llhttp refuses, with every one of its lenient flags off (a bare LF, a stray CR, a space before a
   colon, a folded line, two `Content-Length`s, `Content-Length` with `Transfer-Encoding`), is 400.
3. **`Host`.** Exactly one, and the address the connection arrived on with its port (the port left out only when it is
   80; an IPv6 address in brackets, in any spelling), or `localhost` and the port on a loopback connection, or one of
   the policy's `hosts` and the port. Anything else is 403: so a rebound name, `0.0.0.0` and a missing `Host` are.
4. **Static content.** A `GET` or `HEAD` of a file's path is answered here, 200 and kept alive (unless the request has a
   body), from anywhere and without the token: it is the page, the same for everyone, and it holds no secrets.
5. **Where it comes from.** `Sec-Fetch-Site`, when present, must be `same-origin` or `none` (the user typed the
   address), and `Origin`, when present, `http://` and the `Host` just checked (letter case aside; `null` never is);
   either twice, or anything else, is 403. A body typed as a form (`application/x-www-form-urlencoded`,
   `multipart/form-data`, `text/plain`: what a page may send another site without asking), or two `Content-Type`s, is
   415. So a page from elsewhere gets nothing through, and a preflight gets no permission: no `Access-Control-*` header
   is ever sent.
6. **The route.** A path no route literal and no file has is 404; a method none has for it is 405, with `Allow` (a
   `GET` route allows `HEAD`; `OPTIONS` is a method like the others).
7. **The token**, when the policy has one: exactly one `Authorization: Bearer <token>` (the scheme in any letter case),
   compared in constant time, or 401 with `WWW-Authenticate: Bearer`.
8. **The body**, only now: a `Content-Length` over `maxRequestBytes` is 413, known before a byte of it is read; the
   whole request within `requestTimeoutMs` (408).
9. **A slot.** None free is 503 with `Retry-After: 1`, kept alive. Otherwise the request is published to the program,
   which has `replyTimeoutMs` to answer, or the library answers 503 with `Retry-After: 1` for it.

Every response says `HTTP/1.1` and carries `X-Content-Type-Options: nosniff` and `Cross-Origin-Resource-Policy:
same-origin`; a program's reply and the library's refusals `Cache-Control: no-store`; a static file `X-Frame-Options:
DENY`, `Content-Security-Policy: frame-ancestors 'none'` and `Referrer-Policy: same-origin`, so no other site may frame
the page and steer the user's clicks. Never a `Server` header. A connection carries one request at a time (bytes after
it wait until it is answered), is kept alive after a reply over HTTP/1.1 unless the request said `Connection: close`,
is closed after one over HTTP/1.0, after `idleTimeoutMs` with nothing on it, and when a reply cannot be sent within
`requestTimeoutMs`.

Two facts shaped the rules more than the rest. **Browsers' own defences are not enough, and not everywhere**: Chrome
and Firefox now ask the user before a public page reaches a local address, Safari does not, a navigation is not
covered, and the user can click Allow; the server needs nothing of it and does not count on it. And **fetch metadata is
sent to loopback only**: `Sec-Fetch-Site` goes to `http://127.0.0.1` and `http://localhost`, not to
`http://192.168.1.20`, so on the LAN the `Origin` check and the token carry the weight. (Both from the research's
sources; no browser was run against the server here.)

#### The token, and the LAN

Off loopback a browser's checks vouch for nothing, and anyone on the network can speak to the port. So a listen that
is not loopback needs a token, or `open`, the host saying in so many letters that it means an open port.

- **The token is the host's**, from the system's random source: at least 16 characters (`head -c 18 /dev/urandom |
  base64 | tr '+/' '-_'` makes 24).
- **It travels in the `Authorization` header**, never in a URL the server sees. The host gives the user the page's
  address with the token after a `#` (`http://192.168.1.20:8080/#k=...`, as a link or a QR code): a fragment is never
  sent to a server, a proxy or a `Referer`, and the page's script reads it and puts it on its own requests
  ([examples/remote/page/remote.js](../examples/remote/page/remote.js) does). A header a page sets itself is also one
  no other site can make a browser send without a preflight, which gets no permission.
- **Without TLS it crosses the LAN in the clear.** It keeps out the curious and the automated, not someone who can read
  the network's traffic: anyone on the same Wi-Fi who captures one request has it, and can then send the program
  anything a route takes. There is no TLS yet ([what is not built](#what-is-not-built)).
- **The static page is served without it**: it is code, the same for everyone, and useless without the token.

#### What a browser will not do for a page on the LAN

A page from `http://192.168.1.20` or `http://studio.local` is not a secure context, and browsers withhold from it what
they give `http://127.0.0.1` and `http://localhost`: Web MIDI, `AudioWorklet`, the clipboard, service workers,
`crypto.subtle`, the screen wake lock (a tablet used as a control surface will go to sleep), and more. Sliders, buttons
and `fetch` all work. A control page meant for the LAN is written to need nothing else (the example's is); FMinus
cannot change this short of TLS.

#### What the operating system does

From the research's sources (§6.6 and its appendix B), not tried here, since nothing here listens beyond loopback and
nothing runs on macOS or Windows:

| | Loopback | The LAN |
|---|---|---|
| macOS firewall (when on) | no prompt (secondary sources only) | "accept incoming network connections?", naming the application; an unsigned build is asked again each build |
| macOS App Sandbox | `com.apple.security.network.server` is needed **even for loopback** | the same |
| macOS Local Network privacy | not needed to accept connections | not needed to accept; a browser other than Safari needs it to open the page |
| Windows firewall | no prompt (secondary sources only) | a dialog on the first listen, per executable; a user who is not an administrator gets a block rule whatever they click |
| Linux | nothing; a port below 1024 needs a capability | the distribution's firewall, if any |

- **In a plugin, the executable and the sandbox are the host application's.** FMinus cannot know whether it may listen
  (an Audio Unit may declare `network.client` and has no key for `network.server`), so a failed bind or listen is an
  ordinary outcome: `http_port` says `HTTP_TRANSPORT`, and the program carries on without its page.
- **The port is taken exclusively on Windows** (`SO_EXCLUSIVEADDRUSE`, never `SO_REUSEADDR`, which there would let
  another process bind the same port over it); on Linux and macOS `SO_REUSEADDR` lets a restarted program take its port
  without waiting out `TIME_WAIT`, and does not let a second listener in. An IPv6 socket is always `IPV6_V6ONLY`.
- **`localhost` binds both addresses** because Chrome tries `::1` first and Firefox `127.0.0.1`. If either half's port
  is taken, the listen fails (`HTTP_TRANSPORT`) rather than leave the other half to another process; `::1` is left out
  only when the machine has no IPv6.

#### What the policy does not cover

The policy bounds the library, not the process, as the client's does ([the host's side](#the-hosts-side)): a program
can declare the C library's own `socket`, `bind` and `listen` and call them, and nothing the server checks applies to
a socket the library did not make. Beyond that:

- **Another process on the machine** connects to loopback as the browser does, and nothing distinguishes it from the
  user's `curl`: a host that minds sets a token, on loopback too.
- **The program itself** decides what a route does: the library hands it only requests that passed every check and name
  a route it declared, and the numbers in them are the program's to bound.
- **The LAN without TLS**: the token and the page cross the network in the clear (above).
- **Denial of service** is bounded, not prevented: sixteen connections that each send a request's head a byte at a
  time hold every connection the server has for `requestTimeoutMs`, and can start again; meanwhile every other
  connection is closed unanswered. A control surface for one user is what the limits are for.

### Serving from the driver and executables

Neither has a host, so the person who runs them says where the program may listen, and by default it listens nowhere.

- **`fminus --run`** takes `--web-listen ADDR` (a listen, above), and with it `--web-token T`, `--web-host NAME`
  (repeatable), `--web-root DIR` (a directory whose files it serves, read once as the program starts) and `--web-open`.
  The other limits are the defaults. Each but `--web-listen` without it is an error (`--web-token: needs --web-listen,
  where the program may listen`); any of them without `--run` is an error (an executable reads its own at run time),
  and in a build without the server each says so. A mistake is named by the option it is in (`--web-token:
  server.token has 5 characters (16 to 256)`).

  ```bash
  build-web/bin/fminus examples/remote/remote.fm --run run 600 --web-listen localhost:8080 --web-root examples/remote/page
  ```

- **An executable** (`--emit exe`) of a program that calls `http_listen` reads `FMINUS_WEB_LISTEN` when it starts
  (unset or empty: nowhere), and with it `FMINUS_WEB_TOKEN`, `FMINUS_WEB_HOSTS` (names separated by spaces, commas or
  newlines), `FMINUS_WEB_ROOT` and `FMINUS_WEB_OPEN` (`1`, `0` or empty); without `FMINUS_WEB_LISTEN` the other four are
  not read. A mistake stops it before `main` runs, with the variable it is in and why on stderr and exit status 2
  (`FMINUS_WEB_LISTEN: server.listen '0.0.0.0:0': 0.0.0.0 is the unspecified address: write '*' for every
  interface`). In a build without the server, `FMINUS_WEB_LISTEN` is not read.

**A root directory** (`--web-root`, `FMINUS_WEB_ROOT`, `web::readServerRoot`) is read once, as the program starts:
regular files beneath it only, no symbolic link followed (a link, to a file or a directory, is skipped), a name that
begins with `.` skipped with all beneath it, and a path the server could not serve skipped too, each with a note on
stderr (`fminus: --web-root: skipped '.git': hidden`). An `index.html` is also served at its directory's path (`/`,
`/app`). The type comes from the extension, in any letter case: `html`, `js` and `mjs`, `css` and `txt` (each with
`charset=utf-8`), `json`, `svg`, `png`, `jpg`, `gif`, `ico`, `wasm` and `wav`, anything else
`application/octet-stream`. More than 256 files, 16 MiB, or 4096 entries looked at is an error, not a partial serve.
On Linux and the other POSIX systems the walk holds each directory open and opens what is in it beneath that
descriptor (`openat` with `O_NOFOLLOW`), never by its path from the root again, so someone who can write the root and
swaps a directory for a link to elsewhere while it is read gets the link refused or the real directory read, never a
file outside (`tests/unit/WebServeTest.cpp` races such a swap against the reader). On Windows each file is read by its
path after the listing, so there the root must be writable by no one else while the program starts (never built).

### The server's threads and lifetime

- **No thread of its own.** The listening sockets and the connections are more descriptors in the one I/O thread's
  wait (`curl_multi_poll`, which takes descriptors beside libcurl's own), each connection a non-blocking state machine
  of the I/O thread's alone; the same command queue, the same join. A plugin that serves starts no more threads than one
  that fetches.
- **The I/O thread turns once a millisecond while a request is out** (published, held by the program, or answered and
  not yet sent), so as not to be woken by the audio thread, and otherwise only when a socket, a command or a deadline
  wakes it.
- **Binding happens in `http_listen`**, on the calling thread, before the sockets are handed to the I/O thread: so
  `http_port` is final when `http_listen` returns.
- **Closing** (`http_close`, `Program::stopWeb()`, `~Program`) stops listening at once, gives up every request the
  program holds and has not answered (its reply, whenever it comes, `HTTP_CANCELLED`) and every one it has not taken
  (its client gets nothing, the connection closed), and wakes every thread in `http_accept`. Then it **sends the
  replies the program handed over before it**: each one whose reply returned 0 goes out whole, as the last response on
  its connection (`Connection: close`), and so does a response already going out (a reply, a refusal, a file); then
  the server lets go of its sockets. The I/O thread writes each into its socket as the close begins, never waiting
  on a client, and the closing call waits for them to leave: a client that reads has its reply at once, and **a client
  that does not read holds the close a quarter of a second at most**, after which its connection is closed with as
  much of the reply as its socket took. `stopWeb()` and `~Program` wake every thread in `http_wait` and at a
  `within`'s end meanwhile, not after. The handles stay safe to use (each says closed, or `HTTP_CANCELLED`) until the `Program` is destroyed, which
  waits for every thread in `http_accept` to have left. A thread of the host's in `http_accept(-1)` is woken by
  `stopWeb()`: the host calls it, joins the thread, and then destroys the `Program`.
- **So a reply answered just before the end is sent**: an executable's `main` returning right after its last
  `http_reply`, a run's last route followed by its `defer http_close(server)`, a `Program` destroyed or stopped as soon
  as its loader returns. (Until 2026-11-13 closing closed every connection at once, and such a reply was lost: the
  client saw an empty response.) Which comes first, a reply or the close, is decided by one atomic operation on the
  request: a reply that returned 0 is sent, one that returned `HTTP_CANCELLED` was too late, and nothing is sent twice.

*Why a quarter of a second, and not a setting:* the bound is the host's own wait, in `stopWeb()`, `~Program` or a
loader's `http_close`, and only a client that does not read (or a network too slow for a reply of `maxReplyBytes`)
ever meets it, since every reply is written into its socket as the close begins. No host knows its clients' speed to
set it better, and a constant keeps the policy and its checks as they were.

**Determinism.** A program that takes requests while it plays depends on when they arrive, as it depends on when a key
is pressed: the block a change lands in is the network's. [Determinism](#determinism-and-offline-rendering) above holds
for the server as for the client. An offline bounce has no server: compiled without a listen, `http_port` is
`HTTP_REFUSED` and `http_next` gives nothing, so the same program renders, untouched.

## How it is built

| File | What it is |
|---|---|
| `src/web/Web.h`, `Web.cpp` | the library: the C functions the builtins call (`fminus_web_http_*`), the host's `fminus::web::Policy` and `Context` (`stop()`), the I/O thread |
| `src/web/Policy.h`, `Policy.cpp` | the pattern language, the URL rules, the address classifier |
| `src/web/Roots*.cpp` | the system's root certificates, a file a platform |
| `src/web/Main.cpp` | an executable's context, from `FMINUS_WEB_ALLOW` (and, with the server, `FMINUS_WEB_LISTEN`, `_TOKEN`, `_HOSTS`, `_ROOT`, `_OPEN`) |
| `src/web/Handle.h` | the kind every handle carries first (a request made, a request received, a server), which every C function reads before anything else |
| `src/web/Server.h`, `Server.cpp` | the server (`FMINUS_WEB_SERVER`): routes, the slots, each connection's state machine on llhttp, the nine rules, static files, the replies' formatting, closing's drain (`Servers::drain`); driven by the I/O thread of `Web.cpp` |
| `src/web/ServerPolicy.cpp` | the server's policy: the listen forms, the token, hosts and files, the limits; what counts as loopback, the browsers' bad ports, a servable path, an acceptable `Host` and `Origin` |
| `src/web/ServerRoot.cpp` | `readServerRoot`: a directory's files as static content, for `--web-root` and `FMINUS_WEB_ROOT` |
| `src/web/Sockets.h`, `SocketsPosix.cpp`, `SocketsWindows.cpp` | the server's sockets, the one part that differs between systems (only the Linux path built and run) |
| `src/web/WebSymbols.def` | every function generated code calls, which the JIT defines by address |
| `src/ir/Calls.cpp` | the builtins' table, with `Feature::Web`, their effects, and what each library function touches (`kRuntimeFacts`) |
| `src/ir/Web.cpp` | a URL's checks, `webRequests()`'s metadata, the module's context global, the status codes' names |
| `src/ir/Within.cpp` | a `within` block: its milliseconds, the scope begun, and its end at each way out (`defer`'s mechanism, `Statements.cpp`); its effects in `Effects.cpp` |
| `src/opt/EarlyReturn.cpp` | the loop budget's and the bounds checks' way out of a function that begins a scope: the thread's scopes abandoned first |
| `src/ir/WebServer.cpp` | a route literal's checks, `webRoutes()`'s metadata and the program's route table, a query's name, a reply's format and its values |
| `src/api/FMinus.cpp` | `Options::web` (and `.server`), the `Program`'s context, `webRequests()`, `webRoutes()`, `webPort()`, `stopWeb()` |
| `src/support/Aot.cpp` | an executable's context in its C `main`, and the libraries it links |
| `cmake/FMinusWeb.cmake` | libcurl and Mbed TLS, built at configure time into the deps cache |
| `cmake/FMinusLlhttp.cmake` | llhttp's three C files, fetched into the deps cache and compiled into `fminus_web` (`FMINUS_WEB_SERVER`) |
| `Scripts/pin-web.sh` | the pin lines for new releases of all three (`Scripts/pin-web.sh 8.23.0 4.1.1 9.4.3`): libcurl's tarball and llhttp's tag archive downloaded and hashed, Mbed TLS's digest read from its GitHub release. Bump them on their security releases |
| `tests/unit/WebTest.cpp` | the library against a loopback server of its own (HTTP and HTTPS, with a certificate authority it makes), and programs fetching through the JIT and as executables |
| `tests/unit/HttpTest.cpp` | the builtins in the compiler, in every build |
| `tests/unit/WebServerTest.cpp` | the server at its own level, from C++, before any builtin: the policy and its classifiers, each rule from a raw socket and from the library's client, keep-alive, limits and timeouts, dropped clients, many threads taking, closing with threads waiting, closing's replies sent (whole, beside the requests given up, and a client that does not read held to the bound, by `http_close`, `stop()` and `~Context`), and the wait-free proof (`HeapCalls`, and seccomp's strict mode) |
| `tests/unit/HttpServerTest.cpp` | the server's builtins in the compiler, in every build: effects, the off build's errors, the route, format and name checks at their columns, the route table, the declarations' attributes, `webRoutes()`, the policy checked by `compile()` |
| `tests/unit/WebServeTest.cpp` | programs that serve, through the JIT (an audio thread taking requests between blocks, a loader in `http_accept`, `stopWeb()`) and as an executable with its environment; `readServerRoot`, and a directory swapped for a link while it reads |
| `tests/unit/WithinTest.cpp` | `within` through the JIT against a loopback server of its own (the promise, the deadline, nesting, each way out, `stopWeb()`, a reply and a serve inside one, a URL with holes, the report's script), a block the loop budget or a bounds check stops, the library's abandon and its wait-free proof (`HeapCalls`, and seccomp's strict mode), and the errors |
| `tests/lit/fminus/web*.fm` | the IR, the errors, the driver; `web-off.fm` in a build without the library, `web-server-off.fm` in one without the server |
| `tests/lit/fminus/within*.fm` | a `within`'s IR (its end at each way out, the abandon on the passes' ways out), its errors, and `within-off.fm` in a build without the library |
| `tests/unit/HoleTest.cpp`, `tests/lit/fminus/holes*.fm` | a URL's holes: each mistake an error at its byte, `webRequests()`'s `*`, and a string percent-encoded byte by byte, sent by one program to another that serves on loopback |
| `tests/unit/JsonServeTest.cpp`, `tests/lit/fminus/json-literal-web.fm` | a JSON literal of modules, arrays, enumerations and slices in a reply and `http_reply_json`, its `HTTP_LIMIT`, the wait-free proof of a reply's module, and the research's data work: days fetched at once, written with `file_write` and posted with `http_post` to a loopback server of the test's own, read back there with `json_read`; a POST's literal over `maxBodyBytes` |
| `tests/unit/WebServerTest.cpp`'s last case | `fminus_web_http_post_json` from C++: the text measured, written into the request's body and sent as `application/json`, with and without strings in its URL, its refusals and its limit |
| `examples/fetch/` | the wavetable example's pack fetched from a server its host allows; packs at once in a `within`; a table swapped in by a `when` |
| `examples/remote/` | a synthesiser a browser controls: its page served by the library, its requests taken in `process` |
| `examples/presets/` | named presets kept over HTTP as files: a route's `string` parameter, a file's name with a hole, a body read into a module with `json_read` and the module written back with `json (preset)` |

Recipe 41 in [extending.md](extending.md) walks through the client as a change to the compiler, and recipe 42 the
server.

## What is verified where

- **Linux x86-64** (clang 18, glibc): built and tested, in a release tree with `FMINUS_WEB=ON` and in the default tree
  without it. Every request a test makes stays on the loopback interface, to a server the test starts on 127.0.0.1
  (or to 127.0.0.2, where nothing listens), and no name is ever looked up beyond the machine (checked with strace in a
  network namespace with loopback alone); HTTPS is tested with a certificate authority the test makes. The library's
  own tests also ran under GCC's ThreadSanitizer and AddressSanitizer, and so did `stop()` racing `http_close`, waiting
  threads and new requests, in a harness outside the tree (libcurl and Mbed TLS uninstrumented).
- **The server, on Linux x86-64**: built and tested in a release tree with `FMINUS_WEB=ON` and `FMINUS_WEB_SERVER=ON`,
  in one with the client alone (no llhttp, listen or root symbols in `libfminus_web.a`, and every server builtin the off
  build's error), and in the default tree. Every socket a test binds is on 127.0.0.1, and every connection is made to it
  (checked with strace over the server's tests, the lit tests and `example-remote --check`). The engine's sources,
  llhttp's three files and its test also ran under GCC's AddressSanitizer with UndefinedBehaviorSanitizer and under
  ThreadSanitizer, outside the presets, with a race harness (threads taking and answering while clients come and go) and
  a mutation fuzzer (100,000 damaged requests: no report, and the server still answered a valid request after);
  ThreadSanitizer found one real bug, now fixed (a thread leaving `http_accept` early was not counted, so destroying the
  context could free the server under it). The wait-free builtins are proved so in the library's code and in a JIT'd
  handler's (`HeapCalls`, and seccomp's strict mode). `SocketsPosix.cpp`'s non-Linux path (`fcntl` after `socket`, plain
  `accept`, `MSG_NOSIGNAL` where defined) was compiled here with `__linux__` undefined and passed the server's tests, a
  client that closes before its reply among them; the path as it was died of `SIGPIPE` there. The server's and the
  client's tests passed twenty times over in a network namespace with loopback alone. `examples/remote/` was driven by
  hand through the driver with `curl` on loopback. `serve`, `route` and `reply` (2026-11-13): built and tested in the
  release tree with the server and in the default tree, not in one with the client alone; their wait-free proof is of a
  JIT'd serve (`ServeTest.cpp`); `examples/remote/` through them was driven again through the driver with `curl` on
  loopback; nothing of them ran under a sanitizer. `within` (2026-11-13): built and tested in the release tree with the
  library and its server and in the default tree, not in one with the client alone; its tests (`WithinTest.cpp`, the
  library's and the JIT's, against their own loopback server) and the library's other tests ran under GCC's
  AddressSanitizer with UndefinedBehaviorSanitizer and LeakSanitizer (every case, no report; `HeapCalls` counts nothing
  there, so the abandon's allocation proof only ran in the normal build, its seccomp half under both) and under
  ThreadSanitizer (every case but the five fork-based proofs, which TSan's own thread keeps from exiting: no report,
  three times over), outside the presets, with the compiler's libraries uninstrumented. Holes, a route's `string`
  parameters, a request's text, a JSON literal through the runtime's writer and `%s` (2026-11-13): built and tested in
  the same two trees, not in one with the client alone nor in one without `FMINUS_JSON` (whose errors are written and
  not run); the wait-free proof of a JIT'd serve covers a `string` parameter of the path and of the query,
  `http_header`, `http_path`, `http_query_string`, a JSON literal written in place and `%s`;
  `fminus_web_http_reply_text_s` and the formatter without `%r`, `%R` and `%b` ran under GCC's AddressSanitizer with
  UndefinedBehaviorSanitizer and ThreadSanitizer with the server's tests; `examples/presets/` was driven by its own
  check, on loopback. A JSON literal of arrays, vectors, enumerations and modules, `json (value)`, `json_slice`,
  `http_reply_json` and a POST's literal (2026-11-13): built and tested in the same two trees, not in one with the
  client alone nor in one without `FMINUS_JSON`; the wait-free proof of a JIT'd reply of a module, an array of modules,
  a slice and an enumeration (`JsonServeTest.cpp`); `fminus_web_http_post_json` from C++ and through the JIT, against
  loopback servers of the tests' own, and with the server's and the client's other tests under GCC's AddressSanitizer
  with UndefinedBehaviorSanitizer (every case) and ThreadSanitizer (every case but the fork-based proofs), the JIT's
  tests compiled by clang uninstrumented beside them. Closing's drain (2026-11-13): built and tested in the release
  tree with the server and in the default tree, not in one with the client alone; its tests (`WebServerTest.cpp`'s,
  closing by `http_close`, `stop()` and `~Context`; `ServeTest.cpp`'s, by a program's deferred `http_close`,
  `stopWeb()` and `~Program`; the executable's last reply in `WebServeTest.cpp`; `example-remote --check`'s `run()`)
  with the server's, the client's, `within`'s and `serve`'s other tests ran under GCC's ThreadSanitizer (every case but
  the six fork-based proofs, three times over, and the lifetime cases twenty times more) and AddressSanitizer with
  UndefinedBehaviorSanitizer and LeakSanitizer (every case), no report, the JIT's programs and the compiler's libraries
  uninstrumented; and the report's reproduction, `curl` against the driver on loopback, gives 204 where it gave an empty
  reply. The drain is in the portable part of `Server.cpp`: the sockets' code is unchanged.
- **The wait-free proofs under both sanitizers** (2026-11-13): every proof's child under seccomp's strict mode is now
  `tests/unit/StrictMode.h`'s, which reads the child's result and then ends the child, never waiting for it to end by
  itself, so the fork-based proofs that ThreadSanitizer's own thread kept from exiting (above) run there too. The 18
  proof cases (`WebServerTest.cpp`'s three, `WebServeTest.cpp`'s, `ServeTest.cpp`'s, `JsonServeTest.cpp`'s,
  `WithinTest.cpp`'s, and those of the JSON, XML, document-text and file tests) and the helper's own six
  (`StrictModeTest.cpp`), each in a process of its own as ctest runs a case, ran 20 times under GCC 13's
  ThreadSanitizer (480 runs, every one passed, no report, none longer than a second) and once under AddressSanitizer
  with UndefinedBehaviorSanitizer (no report), outside the presets, with the web library, llhttp and the runtime's own
  sources but `Math.cpp` instrumented, and the test files compiled by g++ from a copy whose `Compile.h` spells `float2`
  and `float4` as GCC's vector types. The proof of `xml_text` as it was, built the same way, still stalled there (its
  parent in `waitpid`, its child a zombie leader beside the sanitizer's thread), stopped after 60 seconds.
- **Not run anywhere yet:** macOS and Windows (their root-certificate loaders, Schannel, the frameworks and system
  libraries an executable links, and the server's sockets there: `SocketsPosix.cpp` with macOS's `SO_NOSIGPIPE`,
  `SocketsWindows.cpp`, curl's handling of extra descriptors on Windows, llhttp built by clang-cl, whose header marks
  its functions `__declspec(dllexport)` on Windows, so a DLL that links `fminus_web` would export them), arm64, the
  `web` preset itself (a `dev` tree: this container builds only the `release` flavour), the sanitizer presets, the
  Python package together with the library, a request to any public host, the IPv6 address check against a real IPv6
  connection, and out-of-memory paths. For the server besides: **IPv6 at all** (this machine's kernel has none, so
  `localhost` has only ever bound 127.0.0.1 and `[::1]` never listened); **any listen that is not loopback** (`*`, a
  LAN address: by rule nothing here binds one, so those paths ran only as the policy's checks and the classifiers of
  `Host` and `Origin`); **a browser** (no page was opened in one: `page/remote.js` was only checked for syntax, and
  run by Node, with a stub of the page's elements, against the program the driver served; what Chrome, Firefox and
  Safari send comes from the research's sources); the pause after `accept` fails for want of descriptors;
  `readServerRoot`'s Windows branch (its limits of 16 MiB and 4096 entries are tested, on Linux); the non-Linux POSIX
  sockets as macOS would build them (only their `fcntl` and plain `accept` path, compiled here with `__linux__`
  undefined, ran: macOS's `SO_NOSIGPIPE` never did); and a sanitizer run of the compiler's and the host's part of the
  server.

## What is not built

- **WebSockets** (the report's §4.5: `ws_open`, `ws_receive_floats`, `ws_send_floats` through wait-free rings). libcurl
  is built without them.
- **What the server leaves out** (the research's §10): **TLS** (a certificate for `192.168.1.20` or `studio.local` is
  the user's problem first, and Windows would need Mbed TLS there), so the LAN is plain HTTP and the token crosses it
  in the clear; **a stream from the server** (server-sent events, WebSockets): a page polls; **chunked request
  bodies**, `Expect: 100-continue` (417), pipelining beyond one request at a time, HTTP/2, compression and ranges;
  **other origins** (CORS): never; **cookies** (a header's text is read, `http_header`, and nothing more of them),
  templating: nothing builds text, by design (a JSON literal and a format write it where it goes); **a JSON value**
  (a literal is an argument only: no document is built, edited or written whole from one), and **a literal's array of
  a length known only when it runs** beyond the first elements of one the program has (`json_slice`); **an "embed this
  file" builtin**: static
  content is the host's; **a server of the host's own** with a ready-made parameter surface: an example host shows the
  way first (`examples/remote/`); and the Python package's binding of it.
- **A disk cache and an `offline` policy** (the report's §8): a host that wants either keeps its own, and hands the
  program the bytes.
- **Writing JSON** for a POST (`json_begin`, `json_set_float`, ...): a POST's body is bytes a program has.
- **A proxy setting**, HTTP/2, compression, cookies and authentication.
- **The Python package** binds `Options::web` (and its `server`), `webRequests()`, `webRoutes()`, `webPort()` and
  `stopWeb()` not yet ([python.md](python.md)).
