# Remote: a synthesiser a browser controls

`remote.fm` is a small synthesiser, a sine, whose gain and pitch a page sets over HTTP and whose meter the page reads
([docs/web.md](../../docs/web.md), "The server"). The program serves: `http_listen()` in `init`, and its
`[[clang::nonblocking]]` `process` takes the requests waiting between two blocks with a `serve` statement, answers each
and only then renders the block, so a change lands at a block boundary on the audio thread, with no lock and no second
thread touching its state. Its routes take typed parameters with their ranges (`route "POST /note/{note}" (int note
[[fminus::range(0, 127)]])`), which the compiled code checks before a route runs (400), and `GET /state` replies with a
JSON literal, `reply 200, json { gain, pitch, level, changes };`. The page, `page/index.html` and `page/remote.js`
(plain HTML and script, nothing fetched from elsewhere, nothing that needs a secure context), is the host's to hand
over: the library serves it itself. It needs a build with the web server and the JSON parser (`cmake --workflow --preset
web`, or `-DFMINUS_WEB=ON -DFMINUS_WEB_SERVER=ON` on any tree); in a build without them it says so and exits 0.

```bash
build-web/bin/example-remote --check                          # what ctest runs: 127.0.0.1, a port the system picks
build-web/bin/example-remote --listen localhost:8080          # serve the page for a minute: open the URL it prints
```

`--check` compiles the program with the default policy first, under which `http_listen` listens nowhere (its port is
`HTTP_REFUSED`, and `webPort()` 0). Then it compiles it again, allowed to listen on `127.0.0.1:0`, with the page as its
static files, lists its routes (and the first's parameter), renders a second at the defaults (0.25, 440 Hz), and starts
an audio thread that calls `process` a block at a time. A second `Program` in the same process, the library's own
client, then fetches the page (served by the library, byte for byte), asks for `/nothing` and `GET /gain` (404 and 405,
from the library: the program never sees them), sends `POST /gain?value=0.5` and `POST /note/81` (204), `?value=nan` and
`/note/128` (the program's 400), and reads `/state` back. Once the audio thread has stopped, a second rendered after the
requests must be heard as they said: a peak of 0.5 at 880 Hz. Then the host calls `stopWeb()`, and nothing listens
there any more. Last, the driver's way (below): the program compiled again, its `run(30)` on a thread of its own, `POST
/stop` sent by the client, and the program destroyed on that thread as soon as `run` returns, maybe before the reply
has gone out; the client must have its 204 all the same (closing sends the replies handed over first: [docs/web.md](
../../docs/web.md), "The server's threads and lifetime"), and `run` must have ended at once.

```
the default policy: the program's port is HTTP_REFUSED, and nothing listens
.../examples/remote/remote.fm:51: POST /gain
.../examples/remote/remote.fm:57: POST /pitch
.../examples/remote/remote.fm:63: POST /note/*
.../examples/remote/remote.fm:69: GET /state
.../examples/remote/remote.fm:103: POST /stop
  float value, from the query, 0 to 1
listening on 127.0.0.1:40267
heard: a peak of 0.2500 at 439 Hz before, 0.5000 at 880 Hz after POST /gain?value=0.5 and POST /note/81
POST /stop: 204, and run() returned at once; the program was destroyed as soon as it had
checked: the page served by the library; 404 and 405 without the program; ...
```

(439: the first second's first upward crossing is its first sample, which is not counted.) Without `--check`, the
harness serves the page where `--listen` says for `--seconds` (60), its audio thread paced at a block of 480 every 10
ms, and prints the URL to open. An address that is not loopback needs `--token`, which the URL then carries after a
`#` (below); the library refuses `*:8080` without one, before anything listens.

## By hand, with the driver

The driver has no audio thread, so `remote.fm` has a function for it: `run(seconds)` renders blocks of 10 ms for about
that long, calling `process` for each, and between two waits up to a block's time with a serve of its own (`serve
(server, 10)`, which blocks) for a route only it has, `POST /stop`, which ends the run at once: the driver destroys the
program as soon as `run` returns, and closing sends the stop's reply first. The requests for `process`'s routes it
leaves waiting for `process`, at the next block (a serve takes only its own routes' requests). It returns the changes
the page made. The page is the directory `--web-root` reads once, as the program starts:

```bash
build-web/bin/fminus examples/remote/remote.fm --run run 600 --web-listen localhost:8080 --web-root examples/remote/page
```

Open `http://localhost:8080/` in a browser on the same machine: the sliders set the gain and the pitch, the buttons a
note, and the meter follows the level (the driver plays nothing: there is no sound to hear, only the numbers the
program keeps). With `curl` (run here, with nothing between it and loopback):

```bash
curl http://localhost:8080/state                              # {"gain":0.25,"pitch":440,"level":0.24999957,"changes":0}
curl -X POST 'http://localhost:8080/gain?value=0.5'           # 204
curl -X POST http://localhost:8080/note/81                    # 204: A5, 880 Hz
curl -X POST 'http://localhost:8080/gain?value=2'             # 400: the program's, out of its fminus::range
curl http://localhost:8080/state                              # {"gain":0.5,"pitch":880.0001,"level":0.48573467,"changes":2}
curl -i http://localhost:8080/gain                            # 405, Allow: POST: the library's
curl -i -H 'Origin: http://evil.example' -X POST 'http://localhost:8080/gain?value=1'   # 403: another site's page
curl -X POST http://localhost:8080/stop                       # 204, and the run ends
```

Stop it with Ctrl-C or `POST /stop`; it prints the changes when its time is up. Without `--web-listen`, `run` returns -4
(`HTTP_REFUSED`) at once: nothing listens unless the person running it says where. In a build without the web server or
the JSON parser, `remote.fm` compiles to nothing, and `--run run` says it has no such function.

From another device on the network, the listen must not be loopback, so it needs a token (or `--web-open`), which the
page reads from its address after a `#` (a browser never sends that part to a server) and sends on each request:

```bash
build-web/bin/fminus examples/remote/remote.fm --run run 600 --web-listen '*:8080' \
    --web-token "$(head -c 18 /dev/urandom | base64 | tr '+/' '-_')" --web-root examples/remote/page
# then open http://<this machine's address>:8080/#k=<the token>
```

On the LAN the page is not a secure context and the token crosses the network in the clear: [docs/web.md](
../../docs/web.md) says what that means. (Not tried here: this example's checks never listen beyond loopback.)

## The program

```c
void answer(http_request r) [[clang::nonblocking]]
{
    if (http_is(r, "POST /gain"))                                    // POST /gain?value=0.5
    {
        const float value = http_query_float(r, "value", -1.0f);
        if (value >= 0.0f && value <= 1.0f) { gain = value; changes += 1; http_reply(r, 204); }
        else http_reply(r, 400);
    }
    ...
    else if (http_is(r, "GET /state"))
        http_reply_text(r, 200, "application/json", "{\"gain\":%g,\"pitch\":%g,\"level\":%.4g,\"changes\":%d}",
                        gain, pitch, level, changes);
}

void process(float* out, int n) [[clang::nonblocking]]
{
    for (http_request r = http_next(server); r; r = http_next(server))
        answer(r);
    // ... the block, with gain and pitch as they now are
}
```

- **The routes are literals**, checked when the program is compiled (a method, one space, a path; `%d` a number of
  the path, read with `http_arg`), and listed for the host before anything runs (`Program::webRoutes()`). The library
  answers 404 for a path none has and 405 for a method none has, so `answer` sees only what it named.
- **Everything in `answer` and `process` is wait-free**: taking a request, matching it, reading its numbers and every
  reply allocate nothing, take no lock and make no system call. `http_accept`, which `serve` uses, blocks, and is an
  error in a `[[clang::nonblocking]]` function.
- **A number from the network is the program's to bound.** `http_query_float` gives the fallback for text that is no
  finite number (`nan`, `inf`), and the program refuses what is out of its range with a 400 of its own.
- **The gain is smoothed** (`heard` follows `gain` over a few milliseconds), so a slider does not click; the pitch
  changes at once, its phase continuous.
- **It gives up determinism**: the block a request lands in depends on when it arrives, as a key pressed does
  ([docs/web.md](../../docs/web.md), "Determinism").
