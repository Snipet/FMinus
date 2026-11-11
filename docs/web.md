# The web library

`FMINUS_WEB` gives FMinus programs an HTTP(S) client: `http_get` and `http_post` start a request and return at once,
`http_wait` blocks until it is done, and the audio thread may read its status, its progress and its body without
waiting. The library is `fminus_web` (`src/web/`), its own static library beside the runtime, on libcurl with Mbed TLS
(Schannel on Windows), and it is off by default. Through it a program reaches only what its host allows: the default
is no network, every URL is a string literal the host can list before anything runs, and the library checks each
request against the host's patterns when it starts, at each redirect and on each connection's address. (The policy is
the library's, not a sandbox: [the host's side](#the-hosts-side) says what it does not cover.)

```c
module Loader
{
    void* request;
    float table[2048];
    int length;

    void load(int preset)                                      // the host calls it off the audio thread
    {
        request = http_get("https://example.com/presets/%d.json", preset);
        if (http_wait(request, 10000) == 200)                  // blocks: refused in a nonblocking function
        {
            void* doc = json_parse(http_body(request), http_size(request));
            length = json_floats(doc, "/table", table, 2048);
            json_free(doc);
        }
        http_close(request);
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

Contents: [building it](#building-it), [the builtins](#the-builtins), [the status codes](#the-status-codes), [the
host's side](#the-hosts-side), [the pattern language](#the-pattern-language), [the driver and
executables](#the-driver-and-executables), [threads and lifetime](#threads-and-lifetime), [determinism and offline
rendering](#determinism-and-offline-rendering), [plugins](#plugins-and-entitlements), [how it is
built](#how-it-is-built), [what is verified where](#what-is-verified-where), [what is not built](#what-is-not-built).
The research behind it, and the choices it made, is [research/web/README.md](research/web/README.md).

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

## The builtins

| Builtin | Does | Effect |
|---|---|---|
| `void* http_get(url, ...)` | starts a GET of the URL and returns its request, a handle, at once | allocates |
| `void* http_post(url, data, bytes, content_type, ...)` | starts a POST of `bytes` bytes at `data` (copied: the buffer is free again when it returns) | allocates |
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
- **A request is an opaque `void*`**, as an FFT plan is. A refused request is a request too, its status already set:
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
- **A body is bytes.** `json_parse`, `xml_parse`, `bytes_floats` and `bytes_pcm16` turn it into numbers; FMinus has no
  strings, and nothing gives a program a body as text.

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

*Why names a program uses without a declaration, and not an `enum` it declares, nor macros:* FMinus never substitutes
a macro in code, so C's way for a library's constants (`EOF`, `errno`'s) is not open, and a builtin enumeration would
give the names a type of FMinus's own that a program cannot name. Constants with an enumeration's behaviour, outside
the file's scope, are what C's `<errno.h>` names would be if they were declarations.

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

**The context.** A program that calls `http_get` or `http_post` has a web context: a global of its module,
`fminus_web_context`, which the host's `Program` sets once when it compiles (as it sets the loop budget's), and which
every request passes to the library. A program given no patterns has none: the global stays null, and the library
refuses every request with no context.

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

- **One I/O thread for the process** drives every request (libcurl's multi interface), started by the first request
  and joined when the last program with a context goes. It is never one of the parallel regions' threads, which run
  at user-interactive priority and must not sit in a socket.
- **Each program has its own context**: its policy, its requests, and its own DNS entries and connections, never
  shared with another program's.
- **A request's result is published with one atomic store**, of its status, after its body and size. From then on the
  reads are wait-free, the body never moves until `http_close`, and the I/O thread never touches the request again.
- **`Program::stopWeb()`** stops the program's network for good: every request in flight ends as `HTTP_CANCELLED`
  before it returns, waking every thread in `http_wait`, and every request started from then on is cancelled at once.
  What completed stays readable. A host that shuts a program down while a thread of its own is inside it (a loader
  blocked in `http_wait`) calls `stopWeb()`, joins that thread, then destroys the `Program`. Destroying a `Program`
  while a thread still runs its code is undefined, as it always is.
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

An offline bounce should not depend on a server. A host that renders offline fetches before it renders (and refuses
to start, or reports, when a fetch failed); a disk cache and an `offline` policy (serve the cache or fail) are the
report's answer, and are not built yet (below).

## Plugins and entitlements

- **A sandboxed host** (an AUv3, an app from the Mac App Store) needs the network client entitlement
  (`com.apple.security.network.client`), which is the host's to have, not FMinus's: a plugin runs in its host's
  process with its host's entitlements. Without it, a connection cannot be made, which the program reads as
  `HTTP_TRANSPORT` (from what the sandbox is documented to do: not tried).
- **No listening socket** is ever opened: there is no server, so no firewall prompt on macOS or Windows.
- **Symbols.** libcurl's and Mbed TLS's are hidden; a plugin that exports only its entry points exports none of them,
  and they cannot clash with another copy in the host process.
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

## How it is built

| File | What it is |
|---|---|
| `src/web/Web.h`, `Web.cpp` | the library: the C functions the builtins call (`fminus_web_http_*`), the host's `fminus::web::Policy` and `Context` (`stop()`), the I/O thread |
| `src/web/Policy.h`, `Policy.cpp` | the pattern language, the URL rules, the address classifier |
| `src/web/Roots*.cpp` | the system's root certificates, a file a platform |
| `src/web/Main.cpp` | an executable's context, from `FMINUS_WEB_ALLOW` |
| `src/web/WebSymbols.def` | every function generated code calls, which the JIT defines by address |
| `src/ir/Calls.cpp` | the builtins' table, with `Feature::Web`, their effects, and what each library function touches (`kRuntimeFacts`) |
| `src/ir/Web.cpp` | a URL's checks, `webRequests()`'s metadata, the module's context global, the status codes' names |
| `src/api/FMinus.cpp` | `Options::web`, the `Program`'s context, `webRequests()`, `stopWeb()` |
| `src/support/Aot.cpp` | an executable's context in its C `main`, and the libraries it links |
| `cmake/FMinusWeb.cmake` | libcurl and Mbed TLS, built at configure time into the deps cache |
| `Scripts/pin-web.sh` | the pin lines for new releases of both (`Scripts/pin-web.sh 8.23.0 4.1.1`): libcurl's tarball downloaded and hashed, Mbed TLS's digest read from its GitHub release. Bump them on their security releases |
| `tests/unit/WebTest.cpp` | the library against a loopback server of its own (HTTP and HTTPS, with a certificate authority it makes), and programs fetching through the JIT and as executables |
| `tests/unit/HttpTest.cpp` | the builtins in the compiler, in every build |
| `tests/lit/fminus/web*.fm` | the IR, the errors, the driver; `web-off.fm` in a build without the library |
| `examples/fetch/` | the wavetable example's pack fetched from a server its host allows |

Recipe 41 in [extending.md](extending.md) walks through it as a change to the compiler.

## What is verified where

- **Linux x86-64** (clang 18, glibc): built and tested, in a release tree with `FMINUS_WEB=ON` and in the default tree
  without it. Every request a test makes stays on the loopback interface, to a server the test starts on 127.0.0.1
  (or to 127.0.0.2, where nothing listens), and no name is ever looked up beyond the machine (checked with strace in a
  network namespace with loopback alone); HTTPS is tested with a certificate authority the test makes. The library's
  own tests also ran under GCC's ThreadSanitizer and AddressSanitizer, and so did `stop()` racing `http_close`, waiting
  threads and new requests, in a harness outside the tree (libcurl and Mbed TLS uninstrumented).
- **Not run anywhere yet:** macOS and Windows (their root-certificate loaders, Schannel, the frameworks and system
  libraries an executable links), arm64, the `web` preset itself (a `dev` tree: this container builds only the
  `release` flavour), the Python package together with the library, a request to any public host, the IPv6 address
  check against a real IPv6 connection, and out-of-memory paths.

## What is not built

- **WebSockets** (the report's §4.5: `ws_open`, `ws_receive_floats`, `ws_send_floats` through wait-free rings). libcurl
  is built without them.
- **A server.** No program or host listens on a socket.
- **A disk cache and an `offline` policy** (the report's §8): a host that wants either keeps its own, and hands the
  program the bytes.
- **Writing JSON** for a POST (`json_begin`, `json_set_float`, ...): a POST's body is bytes a program has.
- **A proxy setting**, HTTP/2, compression, cookies and authentication.
- **The Python package** binds `Options::web`, `webRequests()` and `stopWeb()` not yet ([python.md](python.md)).
