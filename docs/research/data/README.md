# Offline data: tabular files and primitive machine learning

*Plan, 2026-11-20, written against FMinus at `5358de8`. The question asked: propose an initial plan for offline data
processing in FMinus, chiefly reading tabular files and running primitive machine learning on them efficiently, as part
of moving FMinus from a language for DSP to a personal programming language for whole projects. Nothing under `src/`,
`include/`, `tools/` or `cmake/` changed for this note. Two syntax additions were given to Bison on a copy of
`Parser.y` (§5.1, §6.1); nothing was lowered, built or run. §13 says what was checked and what was not.*

## 0. Summary

**What I would build**, in this order. Each step is a recipe or two of [extending.md](../../extending.md), and each is
useful without the ones after it:

1. **A program that is a tool** (§4). `int main(...)`, whose result is the exit status; `string` parameters filled
   from the command line (`int main(string name, float rate)`); C's `printf`, and `eprintf` for stderr. The data steps
   need this first, because every later test and example is a program that prints.
2. **Tables in** (§5). `csv_parse(data, bytes)` turns bytes (a file's, or a response's) into a `csv_doc`.
   `csv_read(doc, rows, n)` fills an array of a module, one row each, using the same table of fields that `json_read`
   uses, with each field named by the header. `csv_floats(doc, column, out, cap)` reads one column. And
   `fminus data.csv --emit fm` writes the row module from a file, as `fminus pedal.cir --emit fm` writes a circuit's.
3. **Computing at the machine's speed, with the same bits every time** (§6). A `reduction(+: ...)` clause on
   `parallel for` (OpenMP's), whose result does not depend on how many threads run it; `sort` and `sort_index`.
   Nothing else is new: `hash` and `randf` are already the random numbers machine learning needs.
4. **Results out** (§7). A CSV literal, `file_write("out/{name}.csv", csv (rows, n))`, written the way the JSON
   literal is; and NumPy's `.npy` in both directions, so Python can plot what FMinus computed.
5. **The algorithms, written in FMinus** (§8). A library of FMinus source, `#include <ml/kmeans.fm>`: statistics,
   scaling, splits, linear and softmax regression, k-means, nearest neighbours, naive Bayes, a decision tree, PCA and
   the metrics. They are not builtins. Each one is tested against numbers computed elsewhere, and each wall it hits is
   a candidate feature for the language.

**What I would not build:** a dataframe or query engine (method chains, lazy plans, SQL), Parquet or Arrow, automatic
differentiation, a GPU path, an owning string, `long`, or a hash table in the language. §10 says why, and what would
bring each one back.

**The five things that decide the design:**

- **The module is the schema.** A row is a module, `module Trip { float fare; int passengers; enum Payment payment; }`,
  filled by name from the header through the table `json_read` already walks (`src/ir/Shapes.cpp`). So the program
  states which columns it expects, a missing column is a status the program tests, and a row is laid out as C lays out
  a struct: four floats side by side are one row of a matrix.
- **Loops, not a dataframe.** FMinus is good at the loop LLVM compiles for the machine it runs on (`poly float`,
  `parallel for`, the inlined math kernels). So the language gets data in and out, lets loops run in parallel without
  races, and stops there. A mean is a loop, and a group-by over an enumeration is an array indexed by it.
- **Reproducible by construction.** FMinus already promises one result for `sinf`, `expf` and `logf` on every machine
  ([language.md](../../language.md)), randomness without state (`randf(n)`), and the same bits from a `parallel for`
  on any number of threads. Training a model is where that is worth most, so every new piece keeps it: the parser's
  numbers are correctly rounded, a reduction's order depends only on the trip count, and a sort is stable. The result
  is the same model, bit for bit, from the JIT or an executable, on one thread or sixteen.
- **Offline is not a mode.** The effects already divide the language: what a `[[clang::nonblocking]]` function calls
  is checked, and nothing else is. Everything here allocates or blocks, so it is refused there, with the messages used
  today. Nothing about audio changes, and nothing here needs a build option (§5.6).
- **The library is the test.** `examples/biquad/` was a kernel to grow the language around, and a machine-learning
  library does the same job for data. A builtin is justified only when FMinus code cannot get close to C's speed (a
  CSV parser, a sort).

**Decisions that are yours** (§11 has each with its reasons):

| Decision | What I would choose | The alternative |
|---|---|---|
| Rows or columns | rows: an array of a module, filled by `csv_read`; a single column by `csv_floats` | columns: a module of pointers the document fills; or a dataframe handle |
| A missing number | NaN, unless the field has a `fminus::default` | 0 (`json_read`'s "left as it was"); or the row refused |
| Reductions | the clause, deterministic by blocks | implicit on `+=`; or partials written by hand, as now |
| `main`'s arguments | typed parameters, `string` among them | C's `argc` and `argv` (which need `char**`) |
| Printing | C's `printf`, and `eprintf` | `print(x)`; or only `main`'s result, as now |
| Writing tables | `csv (...)`, a keyword beside `json (...)` | a builtin, `csv_write("name", rows, n)` |
| The library | `lib/`, reached as `#include <ml/...>` | `examples/`, with `-I` and quotes |

## 1. The program this plan is for

Iris, by softmax regression: read a CSV, standardise it, train on four fifths of the rows, report on the rest, and save
the model. Written in the proposed language, every new piece is marked:

```c
// iris.fm: which iris a flower is, from four measurements.
//   fminus iris.fm -o iris
//   FMINUS_FILES_READ=data=$PWD/data FMINUS_FILES_WRITE=out=$PWD/out ./iris iris
enum Species { setosa, versicolor, virginica };          // the file's own words, as json_read reads an enumeration
enum { D = 4, K = 3, SEED = 7 };

module Flower                                            // a row of data/iris.csv            (new: csv::name)
{
    float x [[csv::name("sepal_length", "sepal_width", "petal_length", "petal_width")]] [D];
    enum Species species;
};

// A fifth of the rows, chosen by their index alone: no state, no array, the same on every thread.
bool held_out(int i) { return randf(hash(SEED) ^ i) < -0.6f; }

// Each feature to mean 0 and deviation 1, over the training rows.
void standardise(Flower* rows, int n)
{
    double sum[D], squares[D];
    int count = 0;
    parallel for (int i = 0; i < n; ++i) reduction(+: sum, squares, count)      // new: the clause
        if (!held_out(i))
        {
            count += 1;
            for (int j = 0; j < D; ++j)
            {
                sum[j] += rows[i].x[j];
                squares[j] += rows[i].x[j] * rows[i].x[j];
            }
        }
    float mean[D], scale[D];
    for (int j = 0; j < D; ++j)
    {
        mean[j] = (float)(sum[j] / count);
        scale[j] = (float)(1 / sqrt(squares[j] / count - (sum[j] / count) * (sum[j] / count)));
    }
    parallel for (int i = 0; i < n; ++i)
        for (int j = 0; j < D; ++j)
            rows[i].x[j] = (rows[i].x[j] - mean[j]) * scale[j];
}

module Model
{
    float w[K][D];
    float b[K];

    void probabilities(const float* x, float* p)          // the softmax of the scores, into p
    {
        float top = -3.0e38f;
        for (int k = 0; k < K; ++k)
        {
            p[k] = b[k];
            for (int j = 0; j < D; ++j)
                p[k] += w[k][j] * x[j];
            top = fmaxf(top, p[k]);
        }
        float total = 0;
        for (int k = 0; k < K; ++k)
        {
            p[k] = expf(p[k] - top);
            total += p[k];
        }
        for (int k = 0; k < K; ++k)
            p[k] /= total;
    }

    float step(const Flower* rows, int n, float rate)      // one step of gradient descent; the loss before it
    {
        double loss = 0;
        float gw[K][D], gb[K];
        int count = 0;
        parallel for (int i = 0; i < n; ++i) reduction(+: loss, gw, gb, count)
            if (!held_out(i))
            {
                float p[K];
                probabilities(rows[i].x, p);
                const int y = rows[i].species;
                loss += -logf(p[y]);
                count += 1;
                for (int k = 0; k < K; ++k)
                {
                    const float e = p[k] - (k == y ? 1.0f : 0.0f);
                    for (int j = 0; j < D; ++j)
                        gw[k][j] += e * rows[i].x[j];
                    gb[k] += e;
                }
            }
        for (int k = 0; k < K; ++k)
        {
            for (int j = 0; j < D; ++j)
                w[k][j] -= rate * gw[k][j] / count;
            b[k] -= rate * gb[k] / count;
        }
        return (float)(loss / count);
    }

    float accuracy(const Flower* rows, int n)              // on the held-out rows
    {
        int right = 0, held = 0;
        parallel for (int i = 0; i < n; ++i) reduction(+: right, held)
            if (held_out(i))
            {
                float p[K];
                probabilities(rows[i].x, p);
                int best = 0;
                for (int k = 1; k < K; ++k)
                    if (p[k] > p[best])
                        best = k;
                held += 1;
                if (best == rows[i].species)
                    right += 1;
            }
        return (float)right / held;
    }
};

int main(string name)                                     // new: int main, a string parameter
{
    file_content f = file_read("data/{name}.csv");
    defer file_close(f);
    csv_doc doc = csv_parse(file_data(f), file_size(f));  // new: csv_doc, csv_parse
    defer csv_free(doc);                                  // runs first: the document reads f's bytes
    const int n = csv_rows(doc);                          // a null document has no rows
    if (n == 0)
    {
        eprintf("data/%s.csv: no table there (file status %d)\n", name, file_status(f));     // new
        return 1;
    }
    Flower* rows = malloc(n * sizeof(Flower));
    defer free(rows);
    if (csv_read(doc, rows, n) != n)                      // new: how many rows had every value
    {
        eprintf("data/%s.csv: a row without all five values\n", name);
        return 1;
    }
    standardise(rows, n);
    Model model;
    for (int epoch = 0; epoch <= 300; ++epoch)
    {
        const float loss = model.step(rows, n, 0.5f);
        if (epoch % 50 == 0)
            printf("epoch %3d  loss %.4f\n", epoch, loss);  // new
    }
    printf("held out: %.1f%% right\n", 100 * model.accuracy(rows, n));
    file_write("out/{name}-model.json", json (model));   // today's, in a build with file writing
    return 0;
}
```

The program is about 150 lines, and every piece of it that is not new already exists in FMinus. Today it cannot be
written at all: `main` cannot take the name or print the loss, nothing splits the file into rows, and the three sums
would each be an array of per-row partials added up afterwards.

## 2. What a data program meets today

Taken from the manual, in the order a program would run into them:

| Need | Today | Where |
|---|---|---|
| Name the file on the command line | `main` takes only `float`s and `double`s. A string hole in a file's name exists (`file_read("data/{name}.csv")`), but nothing gives `main` a string | [programs.md](../../programs.md), "`main`" |
| Say what happened | `main` prints its one result, and there is no `printf`. The `%g` formatter exists only in the web server (`src/web/Server.cpp`, `http_reply_text`) | programs.md, "Where to go next" |
| An exit status | none: 0, or 2, 3 or 4 for the executable's own failures | programs.md |
| Read a CSV | `file_read` gives bytes, and `string_*` reads one field at a time; nothing splits rows and fields or handles quotes | [files.md](../../files.md), language.md |
| A file over 256 MiB | the policy's default `maxFileBytes` (`src/runtime/Files.h`), at most the build's ceiling (1 GiB by default, 2 GiB − 2 bytes at most) | files.md, "Building it" |
| A table whose row count is known only at run time | `malloc(n * sizeof(Row))` and a pointer: fine. A 2-D array's sizes are constants, so a matrix of n rows is a `float*` indexed `i * d + j` | language.md |
| A sum over rows on every core | `parallel for` has no reductions: "an iteration writes its own element, and the code after the loop combines them" | language.md; extending.md, recipe 22 |
| A sort, a median, a split by rank | nothing: C's `qsort` takes a function pointer, and FMinus has none | |
| Random initial weights, a shuffle | `hash(n)` and `randf(n)`, already right (§6.3) | language.md |
| exp, log, tanh for activations | `expf`, `logf` and `tanhf`, FMinus's kernels, with one result on every machine | language.md |
| Write a result as a table | `file_write` of bytes or a JSON literal; JSON Lines with `file_append` | files.md |
| Counts past 2^31 | `int` is 32 bits and FMinus has no `long`; sizes are `int`s (2 GB per block) | language.md |

Most of the "Today" column reflects FMinus's role as a host's language: it was designed as a function running inside
someone else's process. A tool is the other way round, with FMinus as the process, and §4 makes that turn.

## 3. The steps at a glance

| Step | New in the language | New in the runtime | The driver |
|---|---|---|---|
| 1. A tool (§4) | `int main`, `string` parameters to `main`, `printf`, `eprintf` | the formatter, moved out of the web server | `--run` passes strings |
| 2. Tables in (§5) | `csv_doc`, `csv_*` builtins, `csv::` attributes | `Csv.cpp`: the reader, and rows by the shape tables; `.npy` reading | `--emit fm` of a `.csv` |
| 3. Computing (§6) | `reduction(...)` on `parallel for`; `sort`, `sort_index` | blocks and a fixed tree in `Parallel.cpp`; a stable sort | |
| 4. Results out (§7) | `csv (...)` and `npy (...)` as sinks' arguments | a CSV writer beside `JsonWrite.cpp`; `.npy` writing | |
| 5. The library (§8) | nothing: it is FMinus source | | `#include <...>` finds it |

## 4. Step 1: a program that is a tool

### 4.1 `main`

```c
int main(string name, float rate)        // ./train iris 0.5
```

- **`int main` returns the exit status**, as C's does, and prints nothing. A `main` returning `float`, `double` or
  `bool` still prints its result, so no existing program changes.
- **A `string` parameter is the argument's text**, a view of `argv`'s bytes, which live as long as the process. The
  executable still reads one argument per parameter; a `string` parameter accepts any text. `--run main iris 0.5`
  passes it in the JIT.
- language.md refuses a `string` in `main`'s signature because C's ABI passes a struct by each platform's own rules.
  Both sides of this call are FMinus's (the entry point that `addEntryPoint` writes in `src/support/Aot.cpp`, and the
  call thunk of recipe 36), so `main` can be exempted while the rule stays for every function declared without a
  definition.
- **Not C's `argc` and `argv`:** `argv` is a `char**`, which needs pointers to pointers (programs.md says so), and
  typed parameters already produce the usage message (`./train: 'main' takes 2 arguments, not 1`). This does not
  cover a program that takes any number of arguments; `arg_count()` and `arg(i)` would, later.

### 4.2 `printf` and `eprintf`

```c
printf("epoch %d: loss %.4g, accuracy %5.1f%%\n", epoch, loss, 100 * accuracy);
eprintf("data/%s.csv: %d rows have holes\n", name, n - complete);
```

- **C's `printf`**, its format checked as clang checks it, as `http_reply_text`'s is today: `%d` (an `int`, a `bool`,
  an enumeration's value); `%g`, `%f` and `%e` with a precision (`%.4g`), a width and the `-` flag (`%8.3f`, `%-12s`,
  since a report is usually a table); `%s` (a `string`); and `%%`. FMinus writes every digit itself, in no locale, so
  the text is the same on every machine.
- **`eprintf`** is the same, to stderr. C would write `fprintf(stderr, ...)`, which needs `FILE*`, and FMinus has no
  `<stdio.h>`. Rust's `eprintln!` and Zig's `std.debug.print` are precedents for a name of its own.
- **Effects:** both block (stdout can be a full pipe), so both are refused in a `[[clang::nonblocking]]` function,
  with the message `http_wait` gets.
- **Where the formatter lives:** today in `src/web/Server.cpp` (`http_reply_text`'s `%g` and `%.Ng`), which is built
  only with `FMINUS_WEB_SERVER`. It would move to `src/runtime/` (beside `Format.h`), and the server would call it
  from there. `printf` gets no build option: every build has it.
- **In a host:** a library host's program writes to the process's stdout unless `Program::Options` says otherwise,
  and the Python package would point it at `sys.stdout`. That is a decision for [library.md](../../library.md), not
  for the language.
- No program in the tree declares a function named `printf` or `eprintf` (§13), so making them builtins breaks
  nothing.

### 4.3 Files a tool names

Today's rules already fit a tool, with one rough edge. `file_read("data/{name}.csv")` names the folder `data`, a
literal the host lists, and `name` fills one segment, checked when it is filled in (files.md, "Holes"). An executable
gets its folders from the environment: `FMINUS_FILES_READ=data=$PWD/data ./train iris`. That is right for a program
someone else wrote, but for one's own tools it is a line to remember on every run. I would let the build record
defaults that the environment overrides (`fminus train.fm -o train --files-read data=./data`, resolved against the
working directory when the program runs). Today `--files-*` together with `--emit` is an error. This is a decision
for you (§11).

## 5. Step 2: tables in

### 5.1 The program's side

```c
enum Payment { CARD, CASH, NONE };
module Trip
{
    float distance;
    float fare [[fminus::range(0, 1000)]];
    int passengers [[fminus::default(1)]];
    enum Payment payment;                         // "CARD" or "CASH", or 0, 1, 2: json_read's rule for an enumeration
    float pickup [[csv::name("Pickup Hour")]];
    char vendor[16];                              // its text, copied
};

file_content f = file_read("data/trips.csv");
defer file_close(f);
csv_doc doc = csv_parse(file_data(f), file_size(f));     // the header, and where each row begins
defer csv_free(doc);                                     // runs before file_close: the document reads f's bytes
const int n = csv_rows(doc);
Trip* trips = malloc(n * sizeof(Trip));
defer free(trips);
const int complete = csv_read(doc, trips, n);            // every row written; how many had every field filled
```

| Builtin | Does | Effect |
|---|---|---|
| `csv_doc csv_parse(data, bytes)`, `csv_parse(data, bytes, "\t")` | reads RFC 4180's CSV (the delimiter one byte, given as a literal), with its first row the header: it finds where each row begins, handling quotes, and copies nothing. Null for text that is not CSV (a quote never closed, a row longer than the header) or that has more rows than an `int` counts | allocates |
| `void csv_free(doc)` | frees the document (its row index and its columns' levels) | allocates |
| `int csv_rows(doc)`, `int csv_columns(doc)` | how many rows (not counting the header) and columns; 0 for a null document | wait-free |
| `string csv_name(doc, j)` | column `j`'s name in the header, a view valid until `csv_free` | wait-free |
| `int csv_find(doc, name)` | the column a `string` names, or -1 | wait-free |
| `int csv_floats(doc, column, out, cap)`, `csv_doubles` | one column, by its index or by its name, into the program's array: the row count is returned and the first `cap` rows written; a cell that is no number is NaN | wait-free |
| `int csv_ints(doc, column, out, cap, fallback)` | the same for integers, with the fallback for a cell that is not one | wait-free |
| `int csv_read(doc, rows, cap)` | the first `cap` rows into an array of a module, each field from the column of its name, returning how many rows had every field without a default filled (§5.2), or a negative status | allocates |
| `string csv_label(doc, column, code)` | a category's text (§5.3), until `csv_free` | wait-free |

- **Bytes in, as with `json_parse`.** A document reads the bytes it was parsed from, which must outlive it, and two
  `defer`s in order give that. Copying a 1 GiB file to avoid the rule would double what a large table costs.
  `json_parse` copies because yyjson parses in place; a CSV document only needs to know where each row begins (4
  bytes per row).
- **A `string` names a column at run time**, unlike a JSON path: a column's name is data, not an address the host
  lists. A literal is a string, so `csv_floats(doc, "fare", ...)` remains the common case.
- **The bytes may come from anywhere:** `csv_parse(http_body(r), http_size(r))` reads a table a server sent.

### 5.2 A row, filled through the table of its fields

`csv_read` is `json_read` for rows ([extending.md](../../extending.md), recipe 50), and uses the same table:

- **A field reads the column with its name**, or with the name in `[[csv::name("...")]]` (a namespace beside
  `json::`, checked the same way), compared byte for byte with the header once its quotes are removed. An array field
  reads one column per element: `float x [[csv::name("a", "b", "c", "d")]] [4];`, so a row's features sit side by
  side.
- **Each kind is read as `json_read` reads it:** a number by fast_float, correctly rounded (`src/runtime/Numbers.cpp`,
  the one copy); an `int` from an integer an `int` holds; a `bool` from `true`, `false`, `1` or `0`; an enumeration
  from its constant's name as written, or its number; a `char` array from text that fits with room for a 0; each
  within its `fminus::range`.
- **A cell that is empty, malformed or out of range** takes the field's `fminus::default`. A `float` or `double` field
  without a default gets NaN, where `json_read` would leave the field as it was: a fresh row's 0 would silently skew a
  mean, while NaN shows up in every sum it reaches. Any other field without a default is left as it was. **The result
  is how many rows had every field without a default filled from its cell**, which is `json_read`'s bool counted over
  rows, so `if (csv_read(doc, trips, n) != n)` is the check.
- **A field that no column names** gives a negative status (`CSV_MISSING`), unless the field has a default. A column
  that no field names is ignored, as a JSON member is. `[[csv::skip]]` works like `[[json::skip]]`.
- **Not filled:** a `string` field (it would point into the document, which the rows may outlive; `json_read` has the
  same rule), a pointer, a handle, a `param`, a poly type, or a nested module (a row is flat; a nested module's fields
  could map to `name.field` columns later).
- **Rows are converted in parallel.** Finding where rows begin is one pass in order, because a quote changes what a
  newline means. Converting them is independent row by row, so `csv_read` runs it as a `parallel for` over blocks of
  rows on the pool the language already has (`src/runtime/Parallel.cpp`), with each row written by one thread. The
  rows come out the same on any number of threads.

### 5.3 Text columns

There are three ways, depending on what the program knows:

| The program knows | Field | Reads as |
|---|---|---|
| every value, when it is written | `enum Species species;` | the constant whose name is the cell's text, so `setosa` reads as `setosa`. Iris's own words are an enumeration's constants |
| it wants the text | `char city[32];` | the cell's text, copied, as `json_read` fills a `char` array |
| neither: the values come from the data | `int city [[csv::category]];` | a code from 0 to k − 1, one per distinct text, numbered in the order the texts first appear (pandas' `factorize`); `csv_label(doc, column, code)` gives the text back |

Categories are what keep a hash table out of the language: grouping by one is an array indexed by its code,
`count[trips[i].city] += 1`, and one-hot encoding is a loop. The document keeps each category column's levels, made the
first time a read asks for them, until `csv_free`. Two files can give the same text different codes; joining on text
is in §10.

### 5.4 The schema, written for you

```bash
fminus data/iris.csv --emit fm          # the module that a file's header and contents suggest
```

```c
// iris.csv: 150 rows, 5 columns. Written by fminus --emit fm: check it, and keep it in the program.
enum species_value { setosa, versicolor, virginica };

module IrisRow
{
    float sepal_length;                   // 4.3 to 7.9
    float sepal_width;                    // 2 to 4.4
    float petal_length;                   // 1 to 6.9
    float petal_width;                    // 0.1 to 2.5
    enum species_value species;
};
```

A column of whole numbers becomes an `int`, and a column of other numbers a `float`, or a `double` when some value does
not survive a round trip through a float. A text column with at most 16 distinct values, each an identifier, becomes
an enumeration; one with more becomes a category. A name that is not an identifier, or is a keyword, is cleaned up and
given `[[csv::name("...")]]`. The observed ranges are comments, not `fminus::range`: a range attribute would turn
tomorrow's larger value into a missing one. The command reads the whole file, because a sample could miss the one row
that makes a column a `double`.

This repeats the circuit compiler's move: the driver writes FMinus source that the program keeps (`fminus pedal.cir
--emit fm`), so the file's shape is checked against the program when it compiles, and nothing is inferred while it
runs.

### 5.5 What the parser reads

RFC 4180: fields separated by the delimiter and rows by LF or CRLF; a field in double quotes may hold the delimiter, a
newline, and `""` for a quote. A UTF-8 byte order mark before the header is skipped, and a last row without a newline
is still a row. A row with fewer fields than the header is short, and its missing cells are empty; a row with more
fields is an error of the document (null). The first version has no comments, no other quote character, no escape
character, and no mode without a header (§11).

It is FMinus's own code, in `src/runtime/` beside `Json.cpp`, using fast_float for numbers, so no dependency is added.
Finding rows is a scan for three bytes (`"`, `\n` and `\r`), which `memchr`-style code does at memory speed; SIMD can
come later if a measurement asks for it (§9).

### 5.6 No build option

JSON and XML have build options because each brings a dependency a build may not want (yyjson, pugixml). The CSV
reader is FMinus's own code on fast_float, which every build already compiles, and it reaches nothing by itself: its
bytes come from `file_read` or a response, each under its own policy. So I would give it no option, as the `string_*`
builtins have none. That also avoids another tree to build and test without the feature (CLAUDE.md asks for one for
every feature a build can leave out).

### 5.7 NumPy's `.npy`, read

`npy_floats(data, bytes, out, cap)` and `npy_shape(data, bytes, axis)`: versions 1 to 3 of the format (a magic string,
a header that is a Python dict literal, then the values), with `<f4`, `<f8` and `<i4` values in C order, read into
floats. It is about a page of code, and it is how data comes back from Python (§7.2).

## 6. Step 3: computing

### 6.1 Reductions in `parallel for`

```c
double loss = 0;
float grad[K][D];
parallel for (int i = 0; i < n; ++i) reduction(+: loss, grad)
{
    ...
    loss += -logf(p[y]);
    for (int k = 0; k < K; ++k)
        for (int j = 0; j < D; ++j)
            grad[k][j] += (p[k] - (k == y ? 1.0f : 0.0f)) * x[i * D + j];
}
```

- **OpenMP's clause**, written after the loop's head, where OpenMP's pragma would put it, just as `parallel for`
  follows OpenMP: `reduction(op: list)`, and more than one clause is allowed. The operators are `+`, `*`, `min` and
  `max` (OpenMP's names). Each entry in the list is a scalar (`float`, `double`, `int`), a float vector, a whole array
  of one or two dimensions, or a pointer's first elements in OpenMP's array-section form, `g[0:d]` (start and length).
- **Inside the loop, a reduced variable can only be updated:** `v += e` (`*=` for `*`; `v = fminf(v, e)`, `fmaxf`,
  `fmin`, `fmax` or `?:` for min and max), and never read any other way. Today `loss += ...` in a `parallel for` is the
  race error (from `src/ir/Parallel.cpp`'s walker), so no program that compiles today changes meaning: something that
  was refused becomes a reduction when the clause names it.
- **The same bits on any number of threads.** The iterations are cut into blocks whose size depends only on the trip
  count, never on the threads or the machine. Each block starts from the operator's identity and runs its iterations
  in order, and the blocks' partial results are combined pairwise in index order, as a balanced tree, and then into
  the variable's value from before the loop. So `--threads 1` and `--threads 0` give the same sum, as do the JIT and
  an executable, on every machine. It is not the sum a plain `for` gives (strictly left to right), and the manual
  would say so. Pairwise summation's error grows as log n, where left-to-right summation's grows as n, so for a mean
  of a million rows it is the better sum anyway.
- **What it costs:** one copy of the list per block, combined once at the end. A reduction of `grad[0:d]` over n rows
  holds (n / block size) × d values.
- **An array element reached through an index** (`grad[k][j]` above, without the clause) is a race the compiler cannot
  see today ("the compiler sees which variable, not which element"). With the clause it stops being one, and assigning
  a reduced variable any other way becomes an error.
- **The grammar accepts it.** On a copy of `Parser.y`, a `"reduction" "(" op ":" items ")"` clause (one or more)
  between the head and the statement, together with §7.1's addition, took the grammar from 507 states to 541 with no
  conflict (§13). `reduction` becomes a keyword, and no program in the tree uses the word as a name.

The alternative needs no syntax: `+=` to an outer scalar would become a reduction by itself. It cannot cover arrays
(the race the compiler cannot see), and it silently turns an error into a meaning (§11).

### 6.2 Sorting

| Builtin | Does |
|---|---|
| `sort(a, n)` | sorts `a`'s first `n` values ascending, in place: `float*`, `double*` or `int*`, with NaNs last |
| `sort_index(keys, n, index)` | fills `index[0..n)` with the order that sorts `keys`, stable (equal keys keep their order), and so deterministic |

C's `qsort` takes a function pointer, and FMinus has none. An order by index is what column data wants anyway: sort
one column's indexes, then read every column through them. A median is a sort, and so are a quantile, a decision
tree's split points and ROC's AUC. Both builtins allocate (a stable sort needs a buffer).

### 6.3 Random numbers: already done

`randf(hash(seed) ^ i)` gives a weight's initial value, and `hash(hash(seed) ^ i)` gives a row's place in a shuffle
(`sort_index` of the hashes) or its side of a split. There is no state, so whether a row is held out is a function of
its index, computed wherever it is needed, in any order, on any thread, and the same everywhere. A normal deviate is
two uniform ones through Box and Muller, written in the library (§8) with kernels that give one result everywhere.

### 6.4 Matrices

A matrix is a `float*` with its rows, columns and stride, in row-major order, and an array of row modules made of
floats already is one: an `IrisRow*` whose row holds `float x[4]` is an n × 4 matrix with a stride of
`sizeof(IrisRow) / 4`, read with no copy. The library's kernels (dot product, axpy, matrix-vector and matrix-matrix
products, Cholesky) are FMinus code written with `poly float` and `parallel for`, measured against a C++ baseline
before anything becomes a builtin (§9). C's own answer to run-time-sized 2-D arrays is a pointer to a variably
modified type (`float (*X)[d] = malloc(n * sizeof *X); X[i][j]`), which C23 made mandatory again. I would add it once
the library's `i * d + j` starts to hurt (§10).

## 7. Step 4: results out

### 7.1 A CSV literal

```c
file_write("out/{name}-predictions.csv", csv (predictions, n));   // a header, then n rows
file_append("out/log.csv", csv (epoch_row));                        // one row; the header first if the file is empty
```

`csv (rows, n)` and `csv (row)` are arguments, never values, as `json (...)` is. The runtime writes the text into the
file's buffer of exactly the right size, measured first:

- the header is the fields' names, as `csv_read` would read them;
- a number is the shortest digits that read back the same (the JSON writer's rule), and NaN is an empty cell (which
  `csv_read` reads as missing);
- an enumeration is its constant's name, and a `char` array or a `string` is its text, quoted when it holds the
  delimiter, a quote or a newline.

The same module reads and writes, so any file written can be read back. `csv` becomes a keyword, and no program in the
tree uses the word. The grammar took `"csv" "(" expr "," expr ")"` and `"csv" "(" expr ")"` with no conflict.

### 7.2 `.npy`, written

`file_write("out/weights.npy", npy (w, K, D))` writes a C-ordered `<f4` array with the given shape (one or two
dimensions); a `double*` writes `<f8`. Its form is the same as `csv (...)`'s, though this one was not given to Bison
separately. Python reads the file with `numpy.load` and plots it: FMinus computes, Python draws. The Python package
already lends NumPy arrays to FMinus functions for a call ([python.md](../../python.md)), so data can already flow from
Python into FMinus's kernels with nothing new.

## 8. Step 5: the library, in FMinus

| File | What | What it needs |
|---|---|---|
| `ml/stats.fm` | count, mean, variance, min, max and quantiles of a column; a covariance matrix | reductions; `sort` |
| `ml/scale.fm` | standardisation and min-max scaling, fitted then applied | reductions |
| `ml/split.fm` | train/test splits, k folds, shuffles | `hash`, `sort_index` |
| `ml/linear.fm` | least squares by the normal equations (Cholesky, in `double`), ridge, and by gradient descent | matrix kernels, reductions |
| `ml/logistic.fm` | binary and softmax regression by batch gradient descent | reductions, `expf`, `logf` |
| `ml/kmeans.fm` | Lloyd's algorithm with k-means++ seeding | reductions over a k × d array, `randf` |
| `ml/knn.fm` | k nearest neighbours | `parallel for`, a partial sort |
| `ml/bayes.fm` | Gaussian and categorical naive Bayes | reductions indexed by class; categories |
| `ml/tree.fm` | a CART tree of limited depth, and stumps | `sort_index` per feature |
| `ml/pca.fm` | principal components by power iteration with deflation | the covariance, matrix kernels |
| `ml/metrics.fm` | accuracy, a confusion matrix, MSE and R², log loss, ROC AUC | `sort_index` |

- **Where it lives:** `lib/` in the repository, installed beside the runtime, and reached the way C reaches its
  implementation's headers, `#include <ml/kmeans.fm>`. Today that form is an error ("FMinus has no system headers").
  The alternative is `-I` and quotes, which needs nothing new (§11).
- **How it is tested:** each algorithm against numbers that a reference implementation (NumPy, scikit-learn) computed
  once and that the tree keeps, since tests read nothing from the network. The data is small and also kept in the
  tree (iris is 150 rows; the rest is generated with `hash`). Tolerances are justified by each algorithm's arithmetic.
  Each algorithm is also tested against itself, bit for bit, across `--threads 1`, 4 and 0, `-O0` and `-O2`, and the
  JIT and an executable.
- **What it finds:** each algorithm written yields a list of what hurt. I expect `i * d + j` (§6.4), the lack of a
  function pointer for an optimiser that takes a loss function (§10), and `long` for counts. That list, not this note,
  should decide the next language features.

## 9. What to measure

Nothing here was run, so these are measurements to make, not results:

| Question | Measurement | Baseline |
|---|---|---|
| Is `csv_read` fast enough? | MB/s on a generated file (10^7 rows of numbers and text), on one thread and on all | the same file through a short C++ reader on fast_float; pandas' `read_csv` where it is installed |
| Does the row scan limit it? | the scan alone, against `memchr` over the same bytes | |
| Are reductions free? | a sum over 10^8 floats: a `parallel for` with the clause, against partials written by hand (today's way) and a plain `for` | |
| Are FMinus's kernels close to C's? | a 1024 × 1024 matrix-matrix product, against a C++ loop nest at clang -O2 | an optimised BLAS, to know the distance, not to close it |
| Is k-means competitive? | iterations per second on 10^6 × 16 with k = 32 | NumPy's vectorised Lloyd, scikit-learn's |

A kernel becomes a builtin (as the FFT is PFFFT's) only where FMinus stays far from the C++ loop. Distance from a tuned
BLAS is not a reason on its own.

## 10. What I would not build, and what would bring it back

| Not built | Why | What would bring it back |
|---|---|---|
| A dataframe: column objects, method chains, lazy plans, SQL | it is a second language inside the first, and its speed comes from a query engine FMinus would have to write; the loop is FMinus's answer | a program that needs joins and group-bys on several keys, over data too large to handle with loops |
| Parquet, Arrow | Arrow's C++ library, and Parquet's codecs and Thrift, are each larger than FMinus; CSV and `.npy` cover a personal project's interchange | a data source that only comes as Parquet. A small reader of FMinus's own for the plain encodings would be the step before a dependency |
| Automatic differentiation | the first algorithms' gradients are a few lines by hand | Enzyme differentiates LLVM IR, which FMinus produces; whether it builds against FMinus's pinned LLVM (23.1.3) was not checked |
| `long` | rows and sizes fit in an `int` up to 2^31, and no algorithm above needs more | a file over 2 GiB (also `FMINUS_FILES_MAX_BYTES`'s ceiling), or timestamps in milliseconds; C's `long long`, 64 bits everywhere |
| Joins on text, a hash table | categories turn grouping into an array index; two files' codes differ | `csv_recode(doc, column, other, column)`, mapping one file's codes to the other's. A hash table needs generics or function pointers |
| Function pointers | FMinus has none, and the web design chose none ("no callbacks") | an optimiser that takes the loss to minimise, using C's own syntax if they come |
| Variably modified pointers (`float (*X)[d]`) | `i * d + j` works | the library's code; they are C's (C23 made them mandatory) |
| Streaming a file larger than memory | whole files up to the policy's limit cover a personal project's data | the file interface's "reading part of a file" (files.md, "What is not built"), plus a reader that keeps a row's tail between parts |
| Dates and times | a date column is text or a number today | `[[csv::date]] int day` (days since 1970) and `double` seconds, ISO 8601 only |

## 11. Toward a personal language, and the decisions

The question had a second half: FMinus as the language of one's own projects, with the whole pipeline in one's own
hands. This plan adds most of the first things a tool needs (a command line, output, an exit status, data in and out).
It leaves a list to keep, each item C's where C has one:

| Next | Why | C's? |
|---|---|---|
| `snprintf(buf, cap, format, ...)` into a `char` array | text a program builds and keeps, with no owning string: the array is the sink, as a reply's buffer is | yes |
| Debug information | so lldb shows FMinus source (programs.md lists it) | n/a |
| `long long` | counts and offsets past 2^31 | yes |
| Function pointers | callbacks, comparators, an optimiser's loss | yes |
| `float (*X)[d]` | matrices sized at run time | yes (C23) |
| A clock, `clock_seconds()` | timing a run, reporting progress | `clock_gettime`'s |
| `#!` on a first line | `./train.fm iris` runs the file through the JIT | no: a C compiler refuses it |

What stays: the effects, the host's lists of files and addresses, the literal names and their holes. They cost a tool
little (§4.3), they are what make a program auditable, and they keep the plugin side what it is. The rule that text is
made only in sinks stays too, `snprintf` included: being a personal language is not, by itself, a reason for a string
type that allocates. A program that needs one would be.

**The decisions, with reasons:**

| Decision | What I would choose | Why | The alternative |
|---|---|---|---|
| Rows or columns | rows, by `csv_read`; a column by `csv_floats` | it is `json_read`'s design and table, lifted to an array; a row of floats is a matrix row; columns stay one call away | a module of column pointers the document fills (SoA throughout); or a dataframe handle |
| A missing number | NaN, unless the field has a default | 0 is silently wrong in a mean, while NaN shows in every result it reaches | 0 (`json_read`'s rule); or the row refused |
| The attribute that names a column | `csv::name`, beside `json::name` | one file's names and another's differ for the same module | one attribute for both (`fminus::name`) |
| Text the program does not know | `[[csv::category]] int`: codes and labels | grouping and encoding without a hash table in the language | `char` arrays only, with the program's own lookup |
| A build option for CSV | none | no dependency, and it reaches nothing (§5.6) | `FMINUS_CSV`, as JSON has |
| Reductions | the clause, deterministic by blocks | covers arrays; an error today becomes a meaning only where the program asks | implicit on `+=`; or partials by hand, as now |
| A reduction's order | fixed by the trip count: the same on any number of threads, but not a plain `for`'s | reproducibility is the point (§0); pairwise is also the more accurate sum | OpenMP's: unspecified, so it changes with the threads |
| `main`'s arguments | typed parameters, `string` among them | the parsing and usage messages exist already | C's `argc` and `argv` (needs `char**`); or `arg(i)` |
| Printing | C's `printf`, and `eprintf` | C's name and checks; data reports want widths | `print(x)`; or only `main`'s result |
| An executable's folders | defaults recorded when it is built, overridden by the environment | a personal tool runs with the same folders every time | the environment only, as now |
| Writing tables | `csv (...)`, a keyword, beside `json (...)` | the same shape as the JSON sink; names stay `file_write`'s, checked and listed | a builtin, `csv_write("name", rows, n)` |
| Interchange with Python | `.npy` both ways | a page of code each way, no dependency, and NumPy reads it natively | CSV only |
| Where the library lives | `lib/`, reached as `#include <ml/...>` | C's way for an implementation's headers | `examples/`, with `-I` and quotes |
| The library's language | FMinus, with builtins only where measured | the library is the test of the language (§0) | builtins for each algorithm |

## 12. Order and size

| Step | Touches | About the size of |
|---|---|---|
| 0. k-means on iris in today's FMinus, with the data as a constant table | nothing: one program | small; it tests this note's guesses about what hurts before anything is built |
| 1. A tool | `Aot.cpp`'s entry point, the formatter moved from `src/web/Server.cpp` into `src/runtime/`, two builtins, `--run`'s strings | small; recipe 38's format checking is reused |
| 2. Tables in | `src/runtime/Csv.cpp` (new), `Shapes.cpp`'s tables with `csv::` names, the builtins in `Calls.cpp` and `RuntimeSymbols.def`, the driver's `--emit fm` for `.csv`, `.npy` reading | recipes 39 and 50 together (`Json.cpp` 612 lines, `Shapes.cpp` 983, `JsonShapes.h` 202) |
| 3. Reductions and sorting | `Parser.y`, `Ast.h`, `Parallel.cpp`'s walker and lowering, `runtime/Parallel.cpp`'s blocks and tree; `sort` in the runtime | the second half of recipe 22 |
| 4. Results out | a CSV writer beside `JsonWrite.cpp` (296 lines); `.npy` writing | recipe 51 |
| 5. The library | `lib/ml/*.fm`, the reference numbers, tests | ongoing |

Steps 1 and 3 do not depend on step 2, so step 3 can come first if the library starts from step 0's program.

## 13. What was checked, and what was not

- **Read:** [language.md](../../language.md), [programs.md](../../programs.md), most of [files.md](../../files.md),
  the summaries of the [syntax](../syntax/README.md) and [files](../files/README.md) research notes, `Parser.y`'s
  statement and JSON-literal rules, the header comments of `src/ir/Shapes.cpp`, `src/ir/Parallel.cpp` and
  `src/runtime/Parallel.cpp`, `Files.h`'s default `maxFileBytes`, and where `http_reply_text`'s formatter lives
  (`src/web/Server.cpp`).
- **Grammar:** on a copy of `Parser.y`, with Bison 3.8.2 (the pinned version), the tree's grammar has 507 states and
  no conflict. Adding the reduction clause (one or more after a `parallel for`'s head, with items `name`,
  `name[e:e]` and `this->name`) and `csv (e, e)` and `csv (e)` as expressions gives 541 states and no conflict. The
  actions were placeholders; nothing was lowered.
- **Names:** a grep of the tree's `.fm` programs, and of the C++ and Python sources and docs for declarations, found
  no use of `csv`, `reduction`, `printf`, `eprintf`, `npy`, `sort` or `sort_index` as a name in an FMinus program
  (`reduction` appears only as a C++ variable in `src/ir/Calls.cpp`). `table` is used in 22 `.fm` files, which is why
  the handle is `csv_doc` and not `table`.
- **Not checked:** anything about speed (§9 lists what to measure). The NPY format, RFC 4180, OpenMP's reduction
  clause, C23's variably modified types, pandas' `factorize` and Enzyme are described from memory, not re-read at
  their sources. Whether a `string` parameter of `main` fits `Aot.cpp`'s entry point as easily as §4.1 says was not
  checked: I read where `addEntryPoint` is, not how it calls `main`. §1's program was not compiled, since nothing in it
  that is new exists yet, and the parts that do exist were checked against the manual only.
