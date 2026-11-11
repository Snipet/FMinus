# A web server for FMinus programs

*Research, 2026-11-11, written against the web library as [web.md](../../web.md) describes it (the HTTP(S) client,
built the same day) and [the report before it](README.md), which put a server "last or never". The question asked:
how would FMinus do a web server? Nothing under `src/`, `include/` or `cmake/` changed for this note. §11 says what
was measured, what was read at its source, and what was neither.*

## 0. Summary

**What I would build:**

1. **The program serves.** `http_listen()` starts a server where the host said it may, and the program takes
   requests from it and answers them with builtins. Not a server the host runs beside the program (any host can
   write that today), and not a mode of the driver (§3).
2. **The audio thread takes the requests itself**, wait-free: `http_next`, `http_is`, the reads and every
   `http_reply` allocate nothing, take no lock and make no system call, so a control change lands between two blocks
   on the thread that computes them, and no second thread touches the program's state. `http_accept` is the one
   that blocks, for a loader thread (§4).
3. **Routes are literals**, `"PUT /osc/%d/gain"`, checked when the program is compiled and listed for the host
   before it runs. The library answers 404 and 405 itself for what no route names, so only requests the program
   declared ever reach its code (§4.2).
4. **One thread, the one `fminus_web` has**: listening sockets and connections wait in the same `curl_multi_poll` as
   the client's transfers. **llhttp** parses the request's head; every socket, limit and check is FMinus's own
   (§5).
5. **Default-deny, and narrower than the client**: no listening unless the host gives an address; loopback unless it
   gives another; a token for anything that is not loopback; `Host`, `Origin` and `Sec-Fetch-Site` checked on every
   request; no CORS at all; small limits on every size and wait (§6).
6. **Its own build option, `FMINUS_WEB_SERVER`, off by default**, on top of `FMINUS_WEB` (§8).

**The four things that decide the design:**

- **FMinus has no callbacks and no strings.** So a program polls for requests rather than being called with one,
  and sees a request as a route it named, numbers and bytes: never a path as text, a header or a cookie.
- **A local server's attacker is the user's own browser.** Any web page can send requests to `127.0.0.1`, and by
  rebinding a name it can make them look same-origin. Local servers in shipping software have fallen to exactly this
  for years. The defences are known and cheap, and they are why half of this report is §6.
- **The honest limit is the same as the client's:** the policy bounds the library, not the process (a program can
  still declare the C library's own `socket`), and the macOS and Windows socket code cannot be built where this was
  written.
- **A page on the LAN is a lesser page.** Without TLS a browser withholds Web MIDI, audio worklets, the wake lock
  and more from `http://192.168.…`, and the token crosses the network in the clear. Loopback has neither problem.
  The first version says so rather than shipping TLS (§6.5, §10).

**What it costs the audio thread**, measured on a prototype here: about 10 ns a block to find nothing waiting, and
about 180 ns to take a request, match it against three routes, read two numbers and format a reply (§11).

**Decisions that were yours, and what I chose so the work could go on** (each is one line to change):

| Decision | Chosen | The alternative |
|---|---|---|
| Who serves | the program, by builtins | a C++ server class for hosts first |
| Its own build switch | `FMINUS_WEB_SERVER`, off by default | part of `FMINUS_WEB` |
| The parser | llhttp, a new pinned dependency | a parser of FMinus's own, and no dependency |
| The LAN | allowed only with a token, or the host's explicit `open` | loopback only, in the first version |
| Static content | the host hands it over | an "embed this file" builtin; or nothing, and pages are a host's business |
| A reply's text | C's `printf` subset: `%d`, `%g`, `%.Ng`, `%%` | a JSON writer |
| Other origins (CORS) | none, ever | an allow-list in the policy |
| TLS | not in the first version | Mbed TLS on all three systems |

## 1. What a server is for, here

FMinus programs make sound. A server in one is not a web site: it is a **control surface**, the thing a MIDI
controller, an OSC port or a plugin's editor is, reached from a browser.

| Use | Shape | Needs |
|---|---|---|
| A page that shows and controls the program: knobs, a keyboard, meters, from the same machine or a phone on the LAN | a few static files, small `GET`s polled at a meter's rate, small `PUT`s on a gesture | static content, routes, numbers both ways |
| Control from a script, a DAW's shell, `curl`: set a parameter, trigger a note, read a level | one request at a time, text a person can type | routes with numbers in them, a query, a text reply |
| Push data into a running program: a wavetable, an impulse response, a preset | one `POST` with a body, parsed off the audio thread | a body, the parsers and `bytes_floats` FMinus has |
| A headless instrument (a Pi in a rack, an installation) whose only interface is the network | all of the above, started by an executable with no host | the driver and executables: who allows the port |
| Meters and scopes pushed to the page, a clock for a jam | a long-lived stream, the server speaking first | server-sent events or WebSockets: not this report's first version (§10) |
| Audio out over HTTP (a radio stream), a public service | codecs, TLS, scale, authentication of strangers | not FMinus's job: out of scope |

The first three rows are one design: a handful of connections, requests of a few hundred bytes, a reply of a few
numbers, and the same question the client raised, **where does the audio thread stand?** Control that arrives over
HTTP is control like any other: it is wanted at a block boundary, on the thread that computes the block, without a
lock.

## 2. What FMinus has that a server builds on

- **`fminus_web` already has a thread and a loop.** One I/O thread for the process sits in `curl_multi_poll`, takes
  commands from a queue, and publishes each result with one atomic store (`src/web/Web.cpp`). `curl_multi_poll` takes
  extra descriptors beside libcurl's own (`curl_waitfd`), so the same thread can wait on a listening socket and its
  connections: measured here against the tree's libcurl 8.23.0, a listener and a connection both wake it, and
  `curl_multi_wakeup` still does (§11).
- **String literals are builtins' arguments, checked and prepared when the program is compiled.** A URL is one; a
  route can be one. So what the client gave a host, every URL before anything runs (`Program::webRequests()`), a
  server can give too: every route.
- **The effects have three classes** (`WaitFree`, `Blocks`, `Allocates`), checked as errors in `[[clang::nonblocking]]`
  and `[[clang::nonallocating]]` functions, and a test harness that proves a wait-free builtin allocates nothing and
  makes no system call (`tests/unit/HeapCalls.*`).
- **The permission model exists:** a policy the host sets (`Program::Options::web`), a context the program cannot
  name, default-deny, an environment for executables, options for the driver.
- **The host's control channel exists too:** a `param` is the host's to set from any thread, a relaxed atomic the
  method reads when it starts (`Program::setValue`). A module's own fields have no such protection: two
  threads in one module's methods are a C data race.
- **FMinus has no callbacks.** A function's address is the host's to take, not a program's: no builtin takes a
  function, and a program has no function pointers. And it has no strings: nothing gives a program a request's path
  as text.

The last two decide the shape of §3 and §4.

## 3. Who serves: three shapes

| | A. The program serves (builtins) | B. The host serves the program (C++) | C. The driver serves a file (`fminus --serve`) |
|---|---|---|---|
| What it is | `http_listen`, and the program answers requests it takes from the library | a `fminus::web::Server` a host fills with handlers; a ready-made one maps `/parameters/...` to `setValue` | B, inside `fminus`, for a program with no host |
| Program code | a few lines a route | none | none |
| What it can expose | anything the program computes: a meter, a table, a note-on, an upload | what the host API sees: parameters (name, range, default, attributes), fields by offset | the same |
| The audio thread | takes requests itself, wait-free, between blocks (§4) | untouched: `setValue` is already atomic | untouched |
| Works in an executable (`--emit exe`) | yes: the library is linked in | no: there is no host | no |
| New language | route literals, a reply's format | none | none |
| New host API | a policy and two listings | a server class, a handler interface, a parameter mapping to design and freeze | options |

**I would build A, on an engine B could later share.** The reasons:

- **It is the one that needs FMinus.** B is a C++ HTTP server beside FMinus; any host can already write it with any
  library, and the parameter API it would wrap is public. What only FMinus can do is let the program's own code,
  on the audio thread, read a request without waiting, and check the routes when the program is compiled.
- **It serves the program that has no host**, the executable and the driver's `--run`, which B cannot.
- **It is the same shape as the client**: a handle, wait-free reads, one builtin that blocks, the policy the host's.
  A programmer who has read [web.md](../../web.md) knows most of it.
- **B is not lost.** The engine (sockets, parsing, limits, the checks of §6) has nothing of the language in it, and a
  C++ face on it for hosts is a later, smaller step. The ready-made parameter surface is the part of B worth having,
  and it needs decisions that are a host's (which parameters, what a meter is), so I would let an example host show
  it before FMinus freezes it.

What A must not do is make a program a general web server. No strings means no templating, no headers to read, no
cookies; that is a feature here. A program sees a method, a path it named itself, numbers and bytes.

## 4. The program's side

### 4.1 Polling, not callbacks

A server library calls a handler. FMinus has no callbacks, and the handler's thread would be the library's, the one
place a program's fields must not be touched from. So the program **takes** requests, as it polls a response's
status today:

```c
module Synth
{
    void* server;
    float gain;
    float level;                                        // a meter process() keeps
    float table[2048];

    void init() { server = http_listen(); }             // under the host's policy: none by default

    void process(float* out, int n) [[clang::nonblocking]]
    {
        for (void* r = http_next(server); r; r = http_next(server))    // wait-free: the requests waiting, or none
        {
            if (http_is(r, "PUT /gain"))                                // a literal: checked when compiled
            {
                gain = http_query_float(r, "value", gain);              // PUT /gain?value=0.5
                http_reply(r, 204);
            }
            else if (http_is(r, "GET /state"))
                http_reply_text(r, 200, "application/json", "{\"gain\":%g,\"level\":%g}", gain, level);
            else if (http_is(r, "PUT /table/%d"))                       // PUT /table/3, four bytes a float
                http_reply(r, bytes_floats(http_body(r), http_size(r), &table[http_arg(r, 0) & 2047], 1) ? 204 : 400);
            else
                http_reply(r, 404);
        }
        // ... the block, with gain as it now is
    }
};
```

Every request is handled **on the audio thread, between two blocks, with no lock and no allocation**: the change
lands at a block boundary, as a host's parameter change does, and no second thread ever touches `gain`. A program
that would rather serve from a thread of its host's (an upload it must parse with `json_parse`, which allocates)
calls `http_accept(server, timeout_ms)` there instead, the one builtin that blocks.

### 4.2 The builtins

| Builtin | Does | Effect |
|---|---|---|
| `void* http_listen()` | starts the program's server where its host said (§7) and returns it, at once | allocates |
| `int http_port(server)` | the port it listens on, or why it does not: `HTTP_REFUSED` (the host allowed none, the default), `HTTP_TRANSPORT` (the address could not be bound), `HTTP_CANCELLED` (closed, or the host stopped it) | wait-free |
| `void* http_next(server)` | the oldest request waiting, now the caller's to answer, or null | wait-free |
| `void* http_accept(server, timeout_ms)` | the same, waiting up to `timeout_ms` for one (negative: until one comes or the server stops) | blocks |
| `bool http_is(r, "METHOD /path/%d")` | whether the request's method and path are the literal's; each `%d` takes a decimal number of the path | wait-free |
| `int http_arg(r, i)` | the i-th number the last `http_is` that matched took | wait-free |
| `float http_query_float(r, "name", fallback)`, `int http_query_int(r, "name", fallback)` | a number of the query, `?name=0.5`, or the fallback | wait-free |
| `int http_size(r)`, `const void* http_body(r)` | the request's body: the client's two builtins, on a request the program received | wait-free |
| `int http_reply(r, status)` | answers with no body | wait-free |
| `int http_reply_bytes(r, status, data, bytes, "content/type")` | answers with bytes the program has (copied: the buffer is free again at once) | wait-free |
| `int http_reply_text(r, status, "content/type", "format", ...)` | answers with text: C's `printf`, of ints (`%d`) and floats or doubles (`%g`) | wait-free |
| `void http_close(server)` | stops listening, and ends what is waiting | allocates |

- **A request is the caller's from `http_next` until its reply.** One reply each: the handle is dead after it, as a
  client's is after `http_close`. A request no one answers in time is answered by the library (503), so a program
  that stalls cannot hold a browser's connection open for ever.
- **A route is a literal**, a method and a path: `"GET /osc/%d/detune"`. The compiler checks it (a method FMinus
  serves, a path of the characters §6.2 allows, at most 8 `%d`s) and records it, so the host can list every route
  before anything runs (`Program::webRoutes()`), and so **the library itself answers what no route names**: a 404
  for a path no literal has, a 405 with `Allow` for a method none has, without a request ever reaching the program.
  `HEAD` is answered from a `GET` route with the body dropped.
- **A reply's text is a format**, FMinus's second kind, and C's: `%d` an int, `%g` a float or a double as
  `printf` writes it (six significant digits), `%.9g` and `%.17g` the digits that read back the same float or
  double, `%%` a percent sign; checked when compiled, in clang's words for `printf` ("format specifies type 'int'
  but the argument has type 'float'"). It is written by `std::to_chars`, so the same in every locale, with no
  allocation. It is what makes a reply `curl` and a page can read, `"{\"gain\":%g}"`, without a writer of FMinus's
  own design. As in C, a NaN is `nan`, which JSON cannot hold: a program that might have one says so itself.
  Arrays of numbers go as bytes (`http_reply_bytes(r, 200, table, 8192, "application/octet-stream")`, a
  `Float32Array` to a page), which is also what `bytes_floats` reads on the way in.
- **A query's number is a finite one.** `http_query_float` gives the fallback for text that is no number, and for
  `nan` and `inf`: nothing a stranger types into a URL puts a NaN into a filter's state.
- **Handles know what they are.** A request made, a request received and a server are all `void*`; each carries
  its kind, and a builtin given the wrong one does nothing and says `HTTP_INVALID`, where today a wrong handle would
  be read as a request.

### 4.3 How a request reaches the audio thread without a lock

The server keeps a fixed table of **slots**, one a request the program has not answered yet (16 by default). The
I/O thread receives a whole request (head and body, bounded: §6), fills a free slot, and publishes it with one atomic
store. From then on:

| Step | Who | How |
|---|---|---|
| take | the program (`http_next`) | scans the table for the oldest published slot and claims it with one compare-and-swap: a bounded loop, no lock, safe from several threads |
| read | the program | the method, path, query and body never change while the slot is claimed: plain reads |
| answer | the program (`http_reply*`) | writes the status and the body into the slot's own buffer, there since the server started, and publishes with one atomic store |
| send | the I/O thread | sees the store on its next turn, sends, frees the slot |

The I/O thread is not woken for a reply, because waking it is a system call and the audio thread makes none. While
a request is out it turns once a millisecond instead (measured: 1.10 ms a turn, idle), and once a second otherwise,
as today. A reply is therefore sent within about a millisecond of the block that wrote it, and a program that never
serves costs nothing.

*What this buys and costs.* The reply's body is bounded by the slot's buffer (64 KiB by default, the host's to
raise): `http_reply_bytes` of more is refused (`HTTP_LIMIT`), and nothing on the audio thread ever allocates to
answer. Slots a program claims and never answers stay claimed until the server closes: sixteen of them, and the
server answers 503 to everything, which is a bug a program finds at once.

### 4.4 Effects, and what a nonblocking function may do

`http_next`, `http_is`, `http_arg`, the query's reads, `http_size`, `http_body`, `http_port` and every `http_reply`
are wait-free, and `tests/unit/HeapCalls.*` can prove it as it does for the parsers' reads (no allocation, no system
call). `http_accept` blocks. `http_listen` and `http_close` allocate.

Wait-free is not free: `http_is` compares a path of at most a few hundred bytes, and a block that finds sixteen
requests waiting answers sixteen. Both are bounded by the limits of §6, and the manual should say what the bound
is.

### 4.5 Determinism

A program that takes requests while it plays depends on when they arrive, as it depends on when a key is pressed.
circuits.md's promise about block sizes does not cover it, and [web.md](../../web.md) already says so of a program
that polls a download. An offline bounce has no server: `http_port` there is `HTTP_REFUSED`, and `http_next` gives
nothing.

## 5. The engine: whose code reads the bytes

Three questions: which thread, which parser, and whose sockets. Appendix A has the survey of eight candidates.

### 5.1 The thread: the one FMinus has

A plugin should start as few threads as it can, and must join every one before it is unloaded. `fminus_web` has
exactly one, and its wait already takes other descriptors (§2). So the server's listening sockets and its connections
are **more descriptors in the same `curl_multi_poll`**, driven by a non-blocking state machine a connection: no
second thread, no thread a connection, the same command queue, the same join.

That rules out every library that brings its own loop or its own threads, which is most of them.

### 5.2 The parser: llhttp, and nothing else of anyone's

| | llhttp 9.4.3 + FMinus's sockets | cpp-httplib 0.66.0 | a parser of FMinus's own |
|---|---|---|---|
| What it is | Node.js's request parser: three generated C files, no allocation, no globals, no I/O | a whole server in one header of 24,000 lines | a few hundred lines |
| Threads | none | a pool of at least 8, a connection holding one for its whole life; blocking sockets | none |
| Process-wide effects | none | `signal(SIGPIPE, SIG_IGN)` in its constructor; `WSAStartup` at static initialisation | none |
| Security record | three CVEs, 2023 to 2025, each a leniency that allowed request smuggling, each fixed; two hardening releases in 2026 | **15 server-side CVEs between May 2025 and August 2026**, one rated critical and seven high, and "security fixes" in most releases since | none, because no one has looked |
| What FMinus still writes | sockets, every limit, every check of §6, responses | the checks of §6, and a bridge from its threads to §4.3's slots | everything |
| License, release | MIT; `release/v9.4.3`, a tag whose archive holds the generated C | MIT; generated tag archives, nine releases in the six weeks before this note | |

**I recommend llhttp for the request's head and its framing, and FMinus's own code for everything else.**

- **The head is where parsers go wrong**, and where a mistake is a security bug: a stray CR, two `Content-Length`s,
  a space before a colon. llhttp is the most-run parser of exactly this, fuzzed, strict by default (all eleven of
  its lenient flags are off unless set), and its three CVEs were all found because it is examined. A parser of
  FMinus's own would start with none found, which is not the same as none.
- **It is only a parser**, which is what fits: it never touches a socket, so it sits inside the I/O thread's state
  machine, and it never allocates.
- **It enforces no limit of its own**, so FMinus must, and the survey names the lesson: a Node CVE of 2024 was an
  embedder's missing bound on something llhttp will happily keep parsing. Every size in §6.3 is counted in FMinus's
  callbacks, and what FMinus does not serve it refuses at the first callback that shows it: another HTTP version
  than 1.0 or 1.1, a method outside the seven, any `Transfer-Encoding` (so llhttp's chunked machinery is never
  entered), an `Upgrade`.
- **It is built as PFFFT and yyjson are**: its three files compiled into `fminus_web` with hidden symbols, not its
  own CMake (which wants CMake 3.25, builds a shared library by default and writes into its source directory).

**This reverses the earlier report**, which named cpp-httplib "the fallback for a server". Read as a server, at its
source, it is the wrong shape (threads and blocking sockets where FMinus has a loop) and it has the wrong year behind
it. The reasons the client chose libcurl over it apply to its server twice over.

The others, briefly: libmicrohttpd is LGPL and capable, with a mode that lends its descriptors to a foreign loop,
but it has no CMake build, speaks `select`'s `fd_set`s, and fixed smuggling and memory bugs through 2026. CivetWeb
has had no release since 2023 and carries unfixed CVEs. libwebsockets is single-threaded and can use a foreign poll,
but is very large, tells its users not to consume its tags, and reports hundreds of audit fixes in months. libevent's
evhttp and uWebSockets each need to own the loop. picohttpparser is the runner-up: smaller than llhttp, no
advisories, but it leaves `Content-Length` and `Transfer-Encoding` semantics to the embedder, which is the part I
would rather not write.

### 5.3 The sockets: FMinus's own, and the part that cannot be tested here

`accept`, `recv` and `send` on non-blocking sockets are a few hundred lines, and they are the same few hundred lines
on three systems only in outline:

| | Linux | macOS | Windows |
|---|---|---|---|
| A send on a closed connection | `MSG_NOSIGNAL` | `SO_NOSIGPIPE` on the socket | no signal |
| Not inherited by a child | `SOCK_CLOEXEC`, `accept4` | `FD_CLOEXEC` after the call | `WSA_FLAG_NO_HANDLE_INHERIT` |
| Taking the port | `SO_REUSEADDR` (a restart need not wait out `TIME_WAIT`) | the same | **`SO_EXCLUSIVEADDRUSE`, never `SO_REUSEADDR`**, which there lets another process bind the same port |
| An IPv6 listener beside an IPv4 one | `IPV6_V6ONLY` set, always | the same | the same (it is the default there, and only there) |
| In `curl_multi_poll` | `poll` | `poll` | curl attaches each socket to its event with `WSAEventSelect` (which makes it non-blocking), reports a close as both readable and writable, and probes with `send(s, NULL, 0)` before waiting for writability: read at curl 8.23.0's source, not in its documentation |

One rule holds on all three: a closed descriptor must never be left in the array, since curl does not report one.

**Only the Linux column can be built and run where this was written.** The client's Windows and macOS code has the
same standing, and the manual already says so; the server adds more of it, in the code that faces the network.
The first build on each of those systems is a test, not a formality.

### 5.4 The pin

`https://github.com/nodejs/llhttp/archive/refs/tags/release/v9.4.3.tar.gz`, SHA-256
`1eb813c7437b31a87496a1cd3ed79f00746720f5e7e29c79b42c02cb69f36c39` (40,403 bytes): the archive as GitHub serves it,
fetched for this note, and the same bytes `git archive` makes of the tag. It is a generated archive, as Catch2's,
yyjson's and pugixml's pins are, and llhttp moves when Node finds something (9.4.1 to 9.4.3 in three months of
2026): `Scripts/pin-web.sh` should print its line with curl's and Mbed TLS's, and bumping it belongs to the same
habit.

## 6. What a listening socket must defend against

The client's risk was a program reaching what it should not. A server's is the reverse: **everything that can reach
the port is its input**, and on a desktop that is more than it seems. A page in the user's browser, from any site,
can send requests to `127.0.0.1`. Appendix B has the survey and its sources; what it comes to:

### 6.1 The threats, and what stops each

| Threat | How it works | What stops it here |
|---|---|---|
| **DNS rebinding** | a page from `evil.example` re-points its own name at `127.0.0.1`; the browser then treats the local server as that page's own origin, and no cross-origin rule applies. Transmission, Blizzard's updater, Ollama and an MCP SDK each shipped a local server that fell to it | the request still says `Host: evil.example`: **`Host` must be the address the connection actually arrived on, with the port** (or `localhost` on a loopback connection, or a name the host listed). Anything else is refused before it is parsed further |
| **A hostile page's cross-site request** (CSRF, "drive-by localhost") | a page may send a `GET`, a `HEAD` or a form-like `POST` to any address without asking; anything else is preceded by a preflight the server must approve | no approval is ever given (**no CORS headers, ever**); a request that says where it comes from must come from the server's own page (**`Origin`, when present, is the server's own; `Sec-Fetch-Site`, when present, is `same-origin` or `none`**); a body typed as a form (`application/x-www-form-urlencoded`, `multipart/form-data`, `text/plain`) is refused, since no route can read one |
| **Another device on the LAN** (when the host allowed the LAN) | sends any bytes it likes, and can forge every header above | **a token**, which the host chose and gave the user out of band (§6.4); and no LAN at all by default |
| **A slow or greedy client** | holds connections open, sends a head that never ends, a body larger than memory | limits on every size and every wait (§6.3), all enforced on the one I/O thread, where a slow client costs a table entry and no thread |
| **A malformed request** | aims at the parser | a strict parser (§5) and a narrow grammar: what is not plainly a request FMinus serves is a 400 and a closed connection |
| **Another local process** | connects to loopback as the browser does | nothing on loopback distinguishes it from the user's `curl`: a host that minds sets a token. On Windows the listener takes its port exclusively (`SO_EXCLUSIVEADDRUSE`, never `SO_REUSEADDR`, which there lets another process bind over it) |
| **The program itself** | a bug that answers the wrong thing, or never answers | the program sees only requests that passed every check above and name a route it declared; one it never answers is answered 503 for it |

Two facts from the survey shape the rules more than the rest:

- **Browsers' own defences are not enough, and not everywhere.** Chrome (142 and later) and Firefox (153 and later)
  now ask the user before a public page reaches a local address; Safari does not, a main-frame navigation is not
  covered, and the user can click Allow. The specification itself says it "cannot fully solve" attacks on local
  services. The server needs nothing for it and must not count on it.
- **Fetch metadata is sent to loopback only.** `Sec-Fetch-Site` goes to `http://127.0.0.1` and `http://localhost`
  (which browsers treat as secure) and not to `http://192.168.1.20`. So on the LAN the `Origin` check and the token
  carry the weight, and on loopback a request from another port of the same machine reads `same-site`, which is why
  only `same-origin` is accepted.

### 6.2 The rules, in the order a request meets them

1. **The connection.** Refused beyond `maxConnections`. Its socket never blocks, is never inherited by a child
   process, and never raises `SIGPIPE`.
2. **The head.** At most 16 KiB and 64 header fields, within `requestTimeoutMs`. HTTP/1.1 (or 1.0), a method FMinus
   serves (`GET`, `HEAD`, `POST`, `PUT`, `PATCH`, `DELETE`, `OPTIONS`), and a target in origin form (`/path?query`).
   The path is letters, digits, `/`, `-`, `.`, `_` and `~`: no percent-encoding, and no empty, `.` or `..` segment,
   so there is nothing to normalise and nothing two parsers could read differently. The query is printable ASCII,
   and is never decoded. A `Transfer-Encoding` is 501, an `Upgrade` is refused, a body's length comes from
   `Content-Length` alone.
3. **`Host`.** Exactly one, equal to the connection's local address and port, or `localhost` and the port on a
   loopback connection, or one of the policy's `hosts`. So `0.0.0.0`, a rebound name and a missing `Host` are all
   403.
4. **Static content.** A `GET` or `HEAD` of a path the host gave a file for is answered here, from anywhere and
   without the token: it is the page, the same for everyone, and it holds no secrets. Its headers (below) keep
   other sites from framing or embedding it.
5. **Where it comes from.** `Sec-Fetch-Site`, when present, must be `same-origin` or `none` (the user typed the
   address). `Origin`, when present, must be `http://` and the `Host` just checked; `null` never is. A body typed as
   a form is 415. Else 403.
6. **The route.** A path no route literal has is 404; a method none has for that path is 405 with `Allow`
   (`OPTIONS` among them: a preflight gets no permission).
7. **The token**, when the policy has one: `Authorization: Bearer <token>`, compared in constant time, or 401.
8. **The body**, only now: at most `maxRequestBytes` (413, known from its `Content-Length` before a byte of it is
   read), all of it within `requestTimeoutMs`.
9. **A slot.** None free is 503 with `Retry-After`. Otherwise the request is published to the program, which has
   `replyTimeoutMs` to answer.

Every response carries `X-Content-Type-Options: nosniff` and `Cross-Origin-Resource-Policy: same-origin` (no other
site may embed it), a program's reply `Cache-Control: no-store`, and a static file `X-Frame-Options: DENY`,
`Content-Security-Policy: frame-ancestors 'none'` (no other site may frame the page and steer the user's clicks)
and `Referrer-Policy: same-origin`. No `Server` header, and no `Access-Control-*` header of any kind.

### 6.3 The limits

| Limit | Default | For comparison |
|---|---|---|
| A request's head | 16 KiB, 64 fields | Node 16 KiB and 1000; nginx 4 × 8 KiB |
| A request's body | 1 MiB | nginx 1 MiB |
| A whole request, head and body | 10 s | Node 60 s for the head, 300 s in all; Go none unless set, with 5 s the usual advice |
| The program's answer | 2 s | |
| An idle kept-alive connection | 15 s | Node 5 s; nginx 75 s |
| Connections | 16 | |
| Requests waiting on the program | 16 | |
| A reply's body | 64 KiB | |

They are the host's (§7), and small on purpose: this is a control surface with one user.

### 6.4 The token, and the LAN

Off loopback, a browser's checks no longer vouch for anything (§6.1), and anyone on the network can speak to the
port. So **a `listen` address that is not loopback needs a token** (or `open`, the host saying in so many letters
that it means an open port), and `checkPolicy` refuses the policy otherwise.

- The token is the host's: at least 16 characters of `A`–`Z`, `a`–`z`, `0`–`9`, `-` and `_`, from the system's
  random source.
- It travels in the `Authorization` header, never in a URL the server sees. The host gives the user the page's
  address with the token **after a `#`** (`http://192.168.1.20:8080/#k=…`, as a link or a QR code): a fragment is
  never sent to a server, a proxy or a `Referer`, and the page's script reads it and puts it on its own requests.
  A header a page sets itself is also one no other site can send without a preflight.
- Without TLS it crosses the LAN in the clear, so it keeps out the curious and the automated, not someone who can
  read the network's traffic. The manual must say so, and TLS is what would change it (§10).
- The static page itself is served without it: it is code, the same for everyone, and useless without the token.

### 6.5 What a browser will not do on the LAN

A page from `http://192.168.1.20` or `http://studio.local` is not a "secure context", and browsers withhold from it
what they give `http://127.0.0.1`: Web MIDI, `AudioWorklet`, the clipboard, service workers, `crypto.subtle`, the
screen wake lock (a tablet used as a control surface will go to sleep), and more. Sliders, buttons and `fetch` all
work. A control page for the LAN should be written to need nothing else; FMinus cannot fix this short of TLS.

### 6.6 What the operating system does

| | Loopback | The LAN |
|---|---|---|
| macOS firewall (when on) | no prompt, from secondary sources only | "accept incoming network connections?", naming the application; unsigned builds are asked again each build |
| macOS App Sandbox | `com.apple.security.network.server` is needed **even for loopback** (Apple DTS, 2026) | the same |
| macOS Local Network privacy | not needed to accept connections (TN3179) | not needed to accept; a browser other than Safari needs it to open the page |
| Windows firewall | no prompt, from community sources only | a dialog on first listen, per executable; a user who is not an administrator gets a block rule whatever they click |
| Linux | nothing; ports below 1024 need a capability | the distribution's firewall, if any |

- **In a plugin, the executable and the sandbox are the host application's.** FMinus cannot know whether it may
  listen: an Audio Unit may declare `network.client` and has no key for `network.server`. So a failed `bind` or
  `listen` is an ordinary outcome, `http_port` says `HTTP_TRANSPORT`, and a program must carry on without its page.
- **`localhost` means two addresses.** Chrome tries `::1` first and falls back to `127.0.0.1` after 300 ms; Firefox
  tries `127.0.0.1` first. A `listen` of `localhost` binds both, as two sockets with `IPV6_V6ONLY` set on the
  second (its default differs between systems), and carries on with one if the machine has no IPv6.
- **Browsers refuse some ports outright** (6000, 6665 to 6669, 10080 and others of the Fetch standard's list),
  whatever listens there. `checkPolicy` should refuse them too, with that reason, rather than let a host wonder.

## 7. The host's side

**The policy** is the client's, one struct longer (`Program::Options::web.server`). Its default is the client's
default: nothing.

```cpp
struct WebServerPolicy
{
    std::string listen;                    // where the program may listen; empty, the default: nowhere
    std::vector<std::string> hosts;        // names a request's Host may say besides the address ("studio.local")
    std::string token;                     // a secret every request to a route must carry (§6.4)
    bool open = false;                     // off loopback with no token: the host means it
    struct File { std::string path, type, bytes; };
    std::vector<File> files;               // static content the library serves itself: GET path, its type, its bytes
    std::int32_t maxConnections = 16;      // open at once
    std::int32_t maxPending = 16;          // requests the program has not answered yet (the slots of §4.3)
    std::int32_t maxRequestBytes = 1 << 20;   // a request's body
    std::int32_t maxReplyBytes = 64 << 10;    // a reply's body
    std::int32_t requestTimeoutMs = 10000;    // to send a whole request
    std::int32_t replyTimeoutMs = 2000;       // for the program to answer one, before the library answers 503
    std::int32_t idleTimeoutMs = 15000;       // a connection kept alive with nothing on it
};
```

| `listen` | Binds |
|---|---|
| `"127.0.0.1:8080"`, `"[::1]:8080"` | that loopback address |
| `"localhost:8080"` | both loopback addresses, one port (a browser may try either first: §6.6) |
| `"localhost:0"` | both, on a port the system picks: what a plugin with several instances wants |
| `"192.168.1.20:8080"` | one interface of the machine: the LAN (needs `token`, or `open`) |
| `"*:8080"` | every interface (needs `token`, or `open`) |

- **The address is the host's, never the program's.** `http_listen()` takes no argument: a program cannot choose to
  be reachable from the network, and cannot choose a port a host did not give it. It reads where it ended up
  (`http_port`), and so does the host (`Program::webPort()`), to show the URL or open the page.
- **`Program::webRoutes()`** lists every route literal in the program, with its method, its path as a pattern (`%d`
  a `*`), its file, line and column: what the program will answer, before it runs, as `webRequests()` lists what
  it will ask.
- **Static content is the host's too.** A page's HTML, script and style are not the program's business and FMinus
  has no way to hold them (no strings, no files). The host hands them over as bytes with a path and a type, and the
  library serves each itself, on the I/O thread, without the program: a plugin gives the assets it embeds, the
  driver reads a directory once when it starts (`--web-root`). There is no path-to-file mapping at request time at
  all, so no traversal to defend. A general "embed this file" builtin would be the language's way to the same end,
  and is its own decision (§10).
- **Lifetime is the context's.** `Program::stopWeb()` closes the listener and every connection, wakes a thread in
  `http_accept` (it returns null, and `http_port` is `HTTP_CANCELLED`), and the destructor frees the rest: no
  listening socket outlives the `Program`, and none is left when a plugin is unloaded.
- **The driver and executables** have no host, so the person running them says: `fminus --run serve --web-listen
  localhost:8080 [--web-token T] [--web-root DIR] [--web-open]`, and for an executable `FMINUS_WEB_LISTEN`,
  `FMINUS_WEB_TOKEN`, `FMINUS_WEB_ROOT` and `FMINUS_WEB_OPEN` in its environment. Neither listens by default.

## 8. The build-time switch

A listening socket is a bigger step than a client: another attack surface, an entitlement in a sandbox, a question
in an app's review. A vendor who wants `http_get` should not have to ship `listen()`. So the server is **its own
option, `FMINUS_WEB_SERVER`, off by default, and it needs `FMINUS_WEB`** (configure says so otherwise).

| | `FMINUS_WEB_SERVER=OFF` (default) | `ON` (needs `FMINUS_WEB=ON`) |
|---|---|---|
| Fetched and built | nothing more | the request parser, if one is embedded (§5); fast_float, for a query's floats, if the parsers have not fetched it |
| `src/web/` | the server's sources compile to nothing | part of `libfminus_web.a` |
| The builtins | known names: `'http_listen' needs the web server, which this build of FMinus leaves out (FMINUS_WEB_SERVER=OFF)` | work, under the policy |
| `fminus/Config.h` | `FMINUS_HAS_WEB_SERVER 0` | `1` |
| The macro | `__FMINUS_WEB_SERVER__` undefined | defined |
| `Options::web.server` | not read | the policy |
| The `web` preset | | turns it on, so one preset tests the whole library |

`http_size`, `http_body` and `http_close` are the client's builtins and stay with `FMINUS_WEB`.

## 9. Testing without leaving the machine

- **The library's own client is the first test client**: one process, two contexts, `http_get` and `http_post`
  against `http_listen`, over loopback. Every status, limit and timeout is checked from both ends.
- **A raw socket is the second**, for what no well-behaved client sends: a request a byte at a time, a head that
  never ends, a body longer than it says, two requests in one write, a `Host` that is not the server's, a page's
  `Origin` from somewhere else, a connection dropped mid-reply, more connections than the limit.
- **Mutation**, as the parsers had: some hundred thousand requests made by damaging valid ones, under GCC's
  AddressSanitizer and UndefinedBehaviorSanitizer, and the claim and the reply raced under ThreadSanitizer.
- **Wait-free is proved, not asserted**: `HeapCalls` for allocation, and a child under seccomp's strict mode for
  system calls, as `JsonTest.cpp` does it, over `http_next`, `http_is`, `http_arg`, the query's reads and every reply.
- **Through the language**: a program served through the JIT and as an executable, lit tests for the IR, the route
  errors, the driver's options and the build without the server.
- **Nothing leaves loopback**, checked as the client's tests were (strace in a network namespace with loopback
  alone), and no test binds anything but a loopback address.

## 10. What the first version is, and what it leaves

**The first version**, in the order I would build it:

1. The engine in `src/web/`: listening, connections, the request parser and its limits, the checks of §6, the slots,
   static content, behind `FMINUS_WEB_SERVER`; tested from C++ before any builtin exists.
2. The language: the builtins of §4.2 with their effects, route literals and the reply's format checked when
   compiled, the route table, handles that know their kind.
3. The host: `Options::web.server`, `webRoutes()`, `webPort()`, `stopWeb()` and the destructor; the driver's options
   and an executable's environment.
4. The manual ([web.md](../../web.md) gains the server; a recipe in extending.md), and an example a browser can
   open: a small synth, its page, its routes.

**What it leaves, on purpose:**

| Left out | Why | What it would take |
|---|---|---|
| TLS | a certificate for `192.168.1.20` or `studio.local` is the user's problem before it is FMinus's, and Windows would need Mbed TLS built there, where the client uses Schannel | Mbed TLS as the server's TLS on all three, a certificate and key in the policy |
| A stream from the server (server-sent events, WebSockets) | a page polls a meter thirty times a second over one kept-alive connection at no cost that matters on a LAN; a stream is the earlier report's §4.5 rings, and its own design | a ring a reply writes to, a framing for numbers |
| Chunked request bodies, `Expect: 100-continue`, pipelining beyond one request at a time, HTTP/2, compression, ranges | no browser needs them of a control surface | the parser has them, or would |
| Other origins (CORS) | a page elsewhere calling the server is the attack of §6; one that is meant to needs the user's permission in Chrome and Firefox and is blocked in Safari | an allow-list in the policy, and preflights answered |
| Reading a request's headers, cookies of the program's own, templating | FMinus has no strings | nothing: this is the design |
| A JSON writer (`json_put_float(doc, "/a/b", x)`) | `%g` in a reply's format covers a control surface's replies; arrays go as bytes | yyjson's mutable documents, with the paths FMinus already prepares |
| An "embed this file" builtin | static content is the host's (§7); a general embed is a language feature with uses beyond a server | the include resolver reaching the code generator |
| The host's own server (shape B) and a ready-made parameter surface | an example host should show it first | a C++ face on the engine |
| The Python package | it does not bind the client's options either | `Options::web` as keyword arguments |

## 11. What was measured, what was read, and what was neither

**Measured here** (Linux x86-64, an AVX-512 Xeon, the tree's own libcurl 8.23.0 where it matters; scratch programs,
not part of the tree):

| What | Result |
|---|---|
| `curl_multi_poll` with a listening socket and a connection among its extra descriptors | each wakes it and is reported in its `revents`; `curl_multi_wakeup` still wakes it |
| An idle turn of the I/O thread at a 1 ms timeout | 1.10 ms (2000 turns in 2.2 s) |
| An empty `http_next` over a table of 16 slots (a prototype of §4.3) | about 10 ns |
| One request handled: taken, three routes tried, one `%d` and a query's float read, a reply of two floats formatted | about 180 ns |
| A function a program declares as `int socket(int, int, int);` | compiles and opens a socket: the policy is the library's, not a sandbox, as [web.md](../../web.md) says |

**Verified against the real archives**, fetched for this note:

| Pin | SHA-256 | |
|---|---|---|
| llhttp `release/v9.4.3` | `1eb813c7…f36c39`, 40,403 bytes | new; also the bytes `git archive` makes of the tag |
| fast_float 8.3.3, yyjson 0.13.0, pugixml 1.16 | as `cmake/FMinusPins.cmake` has them | the three pins the parsers' work could only reproduce with `git archive`: each now matches the archive GitHub serves, digest and size |

**Read at its source, not run:** the eight candidates of appendix A (repositories, release pages, advisories);
curl 8.23.0's `lib/multi.c` and `lib/select.c` for what `curl_multi_poll` does with extra descriptors on Windows;
the standards, browser release notes and vendor documents of appendix B.

**Neither read at a primary source nor tested:**

- That a loopback-only listener raises no firewall prompt on macOS or Windows: secondary sources agree, no vendor
  document says it.
- Whether a plugin may listen at all inside a sandboxed host, an out-of-process Audio Unit host or an AUv3
  extension: Apple documents the entitlement, not which hosts hold it.
- Safari's behaviour on several points (whether it sends `Sec-Fetch-Site` to `http://localhost`, how it resolves
  `localhost`).
- Any 2026 CVE whose root cause is llhttp: Node's notes for 9.4.2 and 9.4.3 name none.
- Everything about the server on macOS, Windows and arm64, since nothing here can build for them.
- Added binary size and build time: llhttp's three files are 11,000 lines of generated C; nothing was built.

## Appendix A: the survey of servers and parsers

Read 2026-11-11 at each project's repository, release page and advisories.

| Candidate | Latest | License | Build | Threads and loop | Security, 2023 to 2026 | For FMinus |
|---|---|---|---|---|---|---|
| **llhttp** | 9.4.3, 2026-07-30 | MIT | three generated `.c` files in the `release/v*` tag | none: a parser | CVE-2023-30589, CVE-2024-27982, CVE-2025-23167 (before v9 only): smuggling by leniency, all fixed | **the parser** |
| picohttpparser | tag v1.5, 2026-10-27 | MIT or Perl | two files, no CMake | none: stateless | no advisories anywhere | the runner-up; leaves `Content-Length` and `Transfer-Encoding` to the embedder |
| cpp-httplib | 0.66.0, 2026-11-10 | MIT | one header, 24,383 lines | blocking; a pool of max(8, cores − 1) threads, growing fourfold; `SIGPIPE` ignored process-wide | 15 server-side CVEs from May 2025 to August 2026 (one critical, seven high) | no |
| libmicrohttpd | 1.0.10, 2026-08-07 | LGPL-2.1-or-later | autotools and Visual Studio projects; no CMake | none in its external mode, which speaks `fd_set` | smuggling fixed April 2026; "possible crashes or out-of-bounds stack writes" fixed July 2026 | no: the build, and GnuTLS only |
| CivetWeb | 1.16, 2023-04-08 | MIT | CMake | a thread a connection, 50 by default | five CVEs 2025 to 2026, fixes in no release | no |
| libwebsockets | tag v5.1.0, 2026-10-30 | MIT and parts | CMake | single-threaded; a foreign loop or an external poll "not recommended" | eleven CVEs 2025 to 2026, one critical | no: very large, and its tags are not for consuming |
| libevent's evhttp | 2.1.13, 2026-07-01 | BSD-3-Clause | CMake | must own the loop | ten CVEs fixed in 2.1.13, three of them evhttp smuggling | no |
| uSockets, uWebSockets | 0.8.8 (2024), 20.81.0 (2026-10) | Apache-2.0 | Makefile only | owns the loop | none found | no |

Mongoose stays excluded by its license (GPL-2.0-only or commercial), as in the earlier report.

## Appendix B: the survey of threats and platform rules

Read 2026-11-11. What §6 rests on, with where it was read:

| Fact | Source |
|---|---|
| DNS rebinding and its defence: an exact `Host` allow-list with the port | NCC Group, "Preventing DNS Rebinding Attacks" (the Singularity wiki) |
| It keeps happening: Transmission (CVE-2018-5702), Blizzard's update agent (2018), Ollama (CVE-2024-28224), the MCP TypeScript SDK (CVE-2025-66414), "0.0.0.0 Day" (2024) | NCC Group's advisory; the SDK's advisory GHSA-w48q-cv73-mx4w; Oligo Security |
| Which cross-site requests need no preflight: `GET`, `HEAD`, `POST` with a form's or `text/plain` content type and no other header of the page's | the Fetch standard; MDN's CORS guide |
| What `Origin` and `Sec-Fetch-Site` say for each kind of request; `null` origins | the Fetch standard; MDN |
| `Sec-Fetch-*` is sent to potentially trustworthy URLs only (loopback, not a LAN address); another port of the same host is `same-site` | W3C Fetch Metadata (working draft, 2026-09-21); Mozilla bug 1732069 |
| `Sec-Fetch-Site` as a primary CSRF defence with `Origin` as the fallback; never allow `null`; no tokens in URLs | OWASP's CSRF prevention cheat sheet; Go 1.25's `CrossOriginProtection` |
| Local Network Access: Chrome 142 (2025-10-28) and later, Firefox 153 (2026-07-21) and later, not Safari; a mitigation that "cannot fully solve" these attacks; the earlier Private Network Access preflight headers are deprecated | Chrome's release notes and guidance; Firefox's release notes; the WICG specification; WebKit bug 171934 |
| Secure contexts: loopback is one, a LAN address is not; what is withheld | MDN, "Secure contexts" and "Features restricted to secure contexts" |
| macOS: `com.apple.security.network.server` is needed for loopback too; accepting connections needs no Local Network permission; an audio component can declare only `network.client` | Apple's entitlement reference; DTS on the developer forums (2026-06); TN3179 (2026-10-06); TN2247 |
| Windows: the firewall dialog and its per-executable rules; `SO_EXCLUSIVEADDRUSE` for every server | Microsoft Learn, "Windows Firewall rules" and "Using SO_REUSEADDR and SO_EXCLUSIVEADDRUSE" |
| `localhost`: Chrome connects to `::1` first and falls back after 300 ms; Firefox to `127.0.0.1` first | Chromium's `host_resolver_manager.cc` and `transport_connect_job.h`; Firefox's `nsHostResolver.cpp` |
| The ports a browser refuses | the Fetch standard, "bad port" |
| Other servers' limits | nginx's core module reference; Node.js 24's `http` documentation and `_http_server.js`; Go's `net/http` |
| The response headers worth sending | OWASP's HTTP headers cheat sheet; MDN on `Cross-Origin-Resource-Policy` |
