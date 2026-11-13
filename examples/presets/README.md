# Presets: a store of named presets, kept over HTTP as files

`presets.fm` is a small service: a page or a script stores presets by name and reads them back
([docs/web.md](../../docs/web.md), [docs/files.md](../../docs/files.md); the research's service, its §8, as far as this
version goes). A preset has a **name**, which the request's path gives, and a body of JSON whose numbers the program
checks when it arrives and then stores as it came, as the file `presets/<name>.json` in the one folder its host maps:

```c
route "PUT /presets/{name}" (string name, http_request q)      // a string parameter: a whole segment of the path
{
    const int status = store(q, name);                         // json_parse, json_float, file_write
    ...
}

int store(http_request q, string name)
{
    if (!string_starts(http_header(q, "content-type"), "application/json"))
        return 415;
    json_doc doc = json_parse(http_body(q), http_size(q));
    defer json_free(doc);
    const float gain = json_float(doc, "/gain", -1.0f);
    const float cutoff = json_float(doc, "/cutoff", -1.0f);
    if (!doc || gain < 0.0f || gain > 1.0f || cutoff < 20.0f || cutoff > 20000.0f)
        return 400;
    switch (file_write("presets/{name}.json", http_body(q), http_size(q)))   // a hole: the name, checked again
    {
    case FILE_OK: return 204;
    case FILE_INVALID: return 400;                             // a name no file may have: ".hidden", "con"
    default: return 500;
    }
}
```

- **The name is text the program never builds.** It is a view of the request's path (a `string`, valid until the
  reply), passed to a function, compared, and filled into the file's name by a hole, `{name}`, where the runtime checks
  it again by the rules a literal's segments keep (no `/`, no leading `.`, no device name of Windows, ...): one that
  breaks a rule is `FILE_INVALID`, which the program answers 400. A name with a byte the server holds no path to (a
  space) never reaches it: the library answers 400 itself.
- **Its host sees everything it can name before it runs**: five routes (`PUT /presets/*`, its `string` parameter
  `name` from the path), and its files, `presets/*.json`, each hole a `*` (`Program::webRoutes()`, `Program::files()`).
- **What it answers with**: a stored preset's bytes as they are, `GET /presets/{name}`; `GET /presets`, a JSON literal
  of the listing's count and its first file's name, `json { "count": file_count(all), "first": file_name(all, 0) }`
  (a string in a JSON literal is escaped as RFC 8259 asks); `GET /names/{i}`, the `i`-th preset's name, a reply's `%s`;
  and a refused body's 400 with a JSON literal that names the name.
- **It runs on a loader's thread**, not the audio thread: it parses JSON and writes files, which allocate and wait on
  the disk, and its `serve (server, 100)` waits up to 100 ms for a request. A preset is the program's to pick up on the
  audio thread later.

It needs a build with the web server, the JSON parser and file writing (`-DFMINUS_WEB=ON -DFMINUS_WEB_SERVER=ON
-DFMINUS_FILES_WRITE=ON`; the `web` preset has the server, the `files` preset writing); in a build without them it says
so and exits 0.

```bash
build/bin/example-presets --check                                   # what ctest runs
build/bin/example-presets --listen localhost:8080 --folder ./mine   # serve for a minute, the presets in ./mine
```

`--check` makes a directory of its own beside `bin/` (and removes it), maps it as the folder `presets` for writing,
lets the program listen on `127.0.0.1:0`, lists its routes and files, and runs `run(60)` on a thread of its own. A
second `Program` in the same process, the library's own client, then stores two presets by name with `POST` (the
library's client sends `GET` and `POST`, so `presets.fm` takes a preset by either), its URLs with holes
(`http_post("http://127.0.0.1:{port}/presets/{name}", ...)`), checks the files are the bodies as sent and reads them
back; sends a gain of 2 (400, and the stored preset as it was), the names `.hidden` and `con` (400, and no file), a name
with a space (the library's 400) and a body of `text/plain` (415); reads a preset no one stored (404); and lists two,
in their names' order. Then the host calls `stopWeb()`, `run` sees its server closed and returns the presets it
stored, 2. Last it compiles the program with the default policy, under which `run` gives `HTTP_REFUSED` (-4) at once.

```
.../examples/presets/presets.fm:53: PUT /presets/* (string name, from the path) (http_request q, from the request)
.../examples/presets/presets.fm:62: POST /presets/* (string name, from the path) (http_request q, from the request)
.../examples/presets/presets.fm:68: GET /presets/* (string name, from the path)
.../examples/presets/presets.fm:76: GET /presets
.../examples/presets/presets.fm:82: GET /names/* (int i, from the path)
.../examples/presets/presets.fm:30: write presets/*.json
.../examples/presets/presets.fm:70: read presets/*.json
.../examples/presets/presets.fm:78: list presets/*.json
.../examples/presets/presets.fm:84: list presets/*.json
listening on 127.0.0.1:40925, presets in .../build-web/presets-check-498
stored warm-pad and bright.2, read back byte for byte; refused a gain of 2, .hidden, con, a space and text/plain; listed 2
checked: the default policy listens nowhere
```

## By hand, with the driver

```bash
mkdir -p mine
build/bin/fminus examples/presets/presets.fm --run run 600 --web-listen localhost:8080 --files-write presets=./mine
```

Then, from the same machine:

```bash
curl -X PUT --json '{"gain": 0.5, "cutoff": 1200}' http://localhost:8080/presets/warm-pad      # 204
curl http://localhost:8080/presets/warm-pad                    # {"gain": 0.5, "cutoff": 1200}, as it was sent
curl -X PUT --json '{"gain": 2, "cutoff": 1200}' http://localhost:8080/presets/warm-pad        # 400, and why
curl -X PUT --json '{"gain": 0.5, "cutoff": 1200}' http://localhost:8080/presets/.hidden       # 400: no such file name
curl http://localhost:8080/presets                             # {"count":1,"first":"warm-pad.json"}
curl http://localhost:8080/names/0                             # warm-pad
```

(`curl --json` needs curl 7.82; older ones say `-H 'Content-Type: application/json' -d '...'`.) The run ends after
its time; it prints how many presets it stored. Without `--web-listen` it listens nowhere and returns -4 at once;
without `--files-write` every write is `FILE_REFUSED`, which the program answers 500.

## What it does not do yet

A preset's numbers are checked one by one, and what is stored is whatever was sent: reading a body into a module by
its fields, and writing one back as JSON, are the next version's. A listing's names come one request at a time: a JSON
literal has no array of a length known only when it runs.
