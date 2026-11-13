# Fetch: the wavetable's pack, from a server its host allows

`fetch.fm` is the wavetable example's oscillator ([../wavetable/](../wavetable/README.md), `#include`d as it is) with
its pack fetched over HTTP instead of handed to it ([docs/web.md](../../docs/web.md)). `Download::start` starts the
request and returns at once, `Download::received` is the bytes so far, which the audio thread may read, and
`Download::finish`, off the audio thread, waits for the pack and hands its body to `Wavetable::init`. Beside it are the
two other ways a program waits for the network: `countPacks`, a script's, fetches several packs at once in one `within`
block, every request ended where the block ends; and `Live`, the audio thread's, plays at once and swaps the pack's
first table in with a `when` on the block it arrives in. `fetch.cpp` is the
host: it allows the program its server's `/packs/` and nothing else, lists the program's URLs before running anything,
fetches, plays, and checks. It needs a build with the web library and the JSON parser (`cmake --workflow --preset web`,
or `-DFMINUS_WEB=ON` on any tree); in a build without them, and on Windows, whose loopback server is not written, it
says so and exits 0.

```bash
build-web/bin/example-fetch --check                     # what ctest runs: its own server, on 127.0.0.1
build-web/bin/example-fetch --write pack.json           # the pack's JSON, for a server of your own
build-web/bin/example-fetch --port 8000 --pack 1        # fetch http://127.0.0.1:8000/packs/1.json
```

`--check` writes the pack (four band-limited cycles, the wavetable example's), serves it on 127.0.0.1 at an ephemeral
port, fetches it through `Download`, plays a second of it, and compares the samples with the same pack handed to
`Wavetable::init` directly: they must be the same, bit for bit. Then it asks for a pack the server does not have (the
program reads 404 as it would any status), fetches four packs at once through `countPacks` (the server has three), plays
`Live` until its table arrives (silence before, the table from its first sample after, the `when` fired once), and
compiles the program again with the default policy, under which the fetch is `HTTP_REFUSED` and no request reaches the
server. No request leaves the machine.

## Against a server of your own

Any static server on this machine will do. With Python's:

```bash
mkdir -p serve/packs
build-web/bin/example-fetch --write serve/packs/1.json
python3 -m http.server 8000 --bind 127.0.0.1 --directory serve &
build-web/bin/example-fetch --port 8000 --pack 1
```

It prints the URL the program names, as a host would show it before running anything, then what it fetched:

```
.../examples/fetch/fetch.fm:21: GET http://127.0.0.1:*/packs/*.json
.../examples/fetch/fetch.fm:54: GET http://127.0.0.1:*/packs/*.json
.../examples/fetch/fetch.fm:87: GET http://127.0.0.1:*/packs/*.f32
fetched pack 1 from 127.0.0.1:8000 and played 48000 samples
```

`--pack 2` asks for `/packs/2.json`, which that server does not have: `status 404`, and the exit status 1. Stop the
server with `kill %1`.

## The program

```c
#if defined(__FMINUS_WEB__) && defined(__FMINUS_JSON__)
module Download
{
    http_response request;

    void start(int port, int pack)
    {
        request = http_get("http://127.0.0.1:%d/packs/%d.json", port, pack);
    }

    int received() [[clang::nonblocking]] { return http_progress(request); }

    int finish(Wavetable* wavetable, float sampleRate)
    {
        int status = http_wait(request, -1);
        if (status == 200 && !wavetable->init(http_body(request), http_size(request), sampleRate))
            status = 0;
        http_close(request);
        return status;
    }
};

enum { MOST_PACKS = 8 };

int countPacks(int port, int count)
{
    const int packs = count < MOST_PACKS ? count : MOST_PACKS;
    http_response r[MOST_PACKS];
    within (10000)                       // all of them at once: where the block ends, none is still running
    {
        for (int k = 0; k < packs; ++k)
            r[k] = http_get("http://127.0.0.1:%d/packs/%d.json", port, k + 1);
    }
    int found = 0;
    for (int k = 0; k < packs; ++k)
    {
        defer http_close(r[k]);
        if (http_status(r[k]) != 200)
            continue;
        json_doc doc = json_parse(http_body(r[k]), http_size(r[k]));
        defer json_free(doc);
        if (json_count(doc, "/tables") >= 1 && json_int(doc, "/size", 0) >= 2)
            found += 1;
    }
    return found;
}

module Live
{
    ...
    void process(float* restrict output, int n) [[clang::nonblocking]]
    {
        when (http_status(request) == 200)   // once, on the block the table arrives in
        {
            length = bytes_floats(http_body(request), http_size(request), table, 2048);
            ...
        }
        ...                                  // silence until then, the table after
    }
};
#endif
```

- **The URL is a literal**, its port and the pack's number the ints after it. The host sees it as the pattern
  `http://127.0.0.1:*/packs/*.json` before anything runs (`Program::webRequests()`), and allows
  `http://127.0.0.1:<port>/packs/*` (`Options::web.allow`): the same program asking for `/presets/1.json`, or for
  another machine, would be refused when it asked. A real program's URL would be its server's,
  `"https://example.com/packs/%d.json"`, and its host would allow that.
- **`finish` blocks, and is off the audio thread.** `http_wait` in a `[[clang::nonblocking]]` function is an error;
  `received` is allowed there, since the reads are wait-free. Loading the tables here, then playing, keeps the output
  a function of the pack alone: a `process` that swapped tables on the block a response arrived in would not be
  deterministic (docs/web.md, "Determinism and offline rendering").
- **`countPacks` waits for all of them in one place.** A `within (10000) { ... }` block starts its requests at once,
  and where it ends every one of them has ended: 200, the server's other status, a failure, or `HTTP_TIMEOUT` for one
  still running at the deadline, which the block's end cancels. So the loop after it reads final statuses only, and a
  `defer http_close(r[k])` closes each on every path. The block waits, so it is off the audio thread too, as `http_wait`
  is: refused in a `[[clang::nonblocking]]` function.
- **`Live` waits for nothing.** Its `process` is nonblocking, so it may not wait; it looks again each block, and
  `when (http_status(request) == 200)` runs once, on the block the table arrived in, `bytes_floats` copying it
  wait-free. That is the price the note above names: the block it arrives in depends on the network, so two renders
  of the same input differ. A plugin that must sound before its pack loads takes it; one that renders offline loads
  first, as `Download` does.
- **Every failure is a status**, never a trap: `HTTP_REFUSED`, `HTTP_TIMEOUT`, a server's 404, or 0 here for a body that
  is no pack (`Wavetable::init` refused it). `http_close` frees the request and its body; the tables are the
  oscillator's own copy.
