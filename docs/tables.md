# Tables: CSV and NumPy's .npy

FMinus reads tables: a CSV file's rows into an array of a module, one row each, a column into an array of numbers, a
column of text into categories, and NumPy's `.npy` files into floats. The reader is FMinus's own code in the runtime
(`src/runtime/Csv.cpp`, `Npy.cpp`), in every build, with no dependency but fast_float's numbers; it reaches nothing by
itself, since its bytes come from `file_read` or a response, each under its own policy. It is step 2 of the plan for
offline data ([research/data/](research/data/README.md)).

```c
enum Species { setosa, versicolor, virginica };          // the file's own words: a cell names its constant

module Flower                                            // a row of data/iris.csv
{
    float x [[csv::name("sepal_length", "sepal_width", "petal_length", "petal_width")]] [4];
    enum Species species;
};

int main(string name)                                    // ./iris iris
{
    file_content f = file_read("data/{name}.csv");
    defer file_close(f);
    csv_doc doc = csv_parse(file_data(f), file_size(f));  // the header, and where each row begins
    defer csv_free(doc);                                  // runs first: the document reads f's bytes
    const int n = csv_rows(doc);                          // 0 for a file that is not there, or no CSV
    Flower* rows = malloc(n * sizeof(Flower));
    defer free(rows);
    if (csv_read(doc, rows, n) != n)                      // the rows with every value
        eprintf("%s.csv: a row without all five values\n", name);
    ...
}
```

Contents: [a document](#a-document), [the builtins](#the-builtins), [columns](#columns), [rows](#rows),
[categories](#categories), [the schema, written for you](#the-schema-written-for-you), [NumPy's .npy](#numpys-npy),
[the audio thread](#the-audio-thread), [what it costs](#what-it-costs), [how it is built](#how-it-is-built), [what is
not built](#what-is-not-built).

## A document

`csv_parse(data, bytes)` reads RFC 4180's CSV, and gives a `csv_doc`, a handle, or null for text that is no CSV:

- **Fields** are separated by commas, or by the byte a third argument names, a literal of one byte that is not a quote,
  a line end or NUL: `csv_parse(data, bytes, "\t")`, `csv_parse(data, bytes, ";")`.
- **Records** end at LF or CRLF (a CR that is the text's last byte too). An empty line is no record. A last record
  without a line end is a record.
- **A field in double quotes** may hold the delimiter, line ends, and `""` for a quote. A quote that opens a field must
  close at its end: a byte after the closing quote that is no delimiter and no line end makes the text no CSV, and so
  does a quote never closed. A quote inside a field that did not begin with one is a byte like any other.
- **The first record is the header**, its fields the columns' names (unescaped). A UTF-8 byte order mark before it is
  skipped. An empty first line is no header, and no CSV.
- **A record with more fields than the header** makes the text no CSV; one with fewer is short, its missing cells
  empty.

**The document reads the bytes where they lie**: it keeps their address and an index of where each row begins (4 bytes
a row), and copies only the header's names (and, when a read asks, a column's categories). So the bytes must outlive
it, which two `defer`s in order give (`defer file_close(f)` first, then `defer csv_free(doc)`, which runs before it). A
null document is an empty table: `csv_rows` is 0, and every read of a column is `CSV_MISSING`.

## The builtins

| Builtin | Does | Effect |
|---|---|---|
| `csv_doc csv_parse(data, bytes)`, `csv_parse(data, bytes, "\t")` | a document of the bytes at `data` (a `const void*`), or null | allocates |
| `void csv_free(doc)` | frees it, its index, names and categories | allocates |
| `int csv_rows(doc)`, `int csv_columns(doc)` | its rows (the header not counted) and columns; 0 for null | wait-free |
| `string csv_name(doc, j)` | column `j`'s name, a view until `csv_free`; empty outside the header | wait-free |
| `int csv_find(doc, name)` | the first column a `string` names, or -1 | wait-free |
| `int csv_floats(doc, column, out, cap)`, `csv_doubles` | a column's cells as numbers into `out`, the first `cap` rows: the rows there are, or `CSV_MISSING` | wait-free |
| `int csv_ints(doc, column, out, cap, fallback)` | the same as ints, `fallback` for a cell that is no integer an int holds | wait-free |
| `int csv_read(doc, rows, cap)` | the first `cap` rows into an array of a module ([rows](#rows)): how many had every value | allocates |
| `int csv_levels(doc, column)` | how many distinct texts a column holds ([categories](#categories)) | allocates |
| `string csv_label(doc, column, code)` | a category's text, a view until `csv_free`; empty for no such code | allocates |
| `int npy_floats(data, bytes, out, cap)`, `int npy_shape(data, bytes, axis)` | an `.npy` file's values, and its shape ([NumPy's .npy](#numpys-npy)) | wait-free |

`CSV_MISSING` (-1: a column the header does not have) and `CSV_INVALID` (-2: a table or an argument the reader
refuses) are names a program uses without declaring them, as `FILE_MISSING` is. `csv_doc` is a keyword, as the other
handles' names are.

## Columns

**A column is an int, its index from 0, or a `string`, its name** (a literal among them): `csv_floats(doc, 2, x, n)`,
`csv_floats(doc, "fare", x, n)`, `csv_floats(doc, csv_name(other, k), x, n)`. A name is the header's first column of
it, compared byte for byte. A column the header does not have, and any column of a null document, is `CSV_MISSING`,
and nothing is written.

**A number** is XML Schema's syntax, as `Numbers.h` reads it: a sign (`+` too), leading zeros, digits on either side of
a point, an exponent, and `inf`, `infinity` and `nan` in any case; correctly rounded to a float (never a double
narrowed) or a double, in no locale (`1,5` is no number: a decimal comma is not read). Spaces and tabs around it are
allowed, and so are quotes around it. An empty cell, a short row's missing one, and a cell that is no number are none:
NaN in `csv_floats` and `csv_doubles`, `fallback` in `csv_ints`, which takes an integer an `int` holds, written as one
(`7`, `-0`) or not (`7.0`, `1e3`); `1.5` and `3e9` are none.

## Rows

`csv_read(doc, rows, cap)` fills `rows`, a pointer to a module (not `const`), a row each, the first `cap` rows, by a
table of the module's columns the compiler makes once a type (`src/ir/Csv.cpp`, `csvShape`): each field is read from
the column of its name.

**The names.** A field's column is its own name, or its `[[csv::name("...")]]`. A field of a nested module is
`inner.field`; an array's element and a vector's lane `name[k]` (`name[i][j]` for a 2-D array's), or, for a 1-D array or
a vector, the k-th of its `csv::name`'s names when it gives one each:

```c
module Point { float x; float y; };
module Row
{
    float fare [[csv::name("Fare Amount")]];
    Point pickup;                                          // pickup.x, pickup.y
    int counts[3];                                         // counts[0], counts[1], counts[2]
    float x [[csv::name("sepal_length", "sepal_width")]] [2];
    float scratch [[csv::skip]];                           // never read
};
```

These are names C's syntax spells, so no field's own name collides with one. Two fields that would read one column
are an error (`duplicate CSV column 'a' in a module that csv_read fills`).

**Each kind**, as `json_read` reads it:

| Field | A cell fills it with |
|---|---|
| `float`, `double` | a number, as a column's are; NaN when none fills it and it has no default |
| `int` | an integer an `int` holds |
| `bool` | `true` or `false` in any case, `1` or `0` |
| an enumeration's value | its constant's name as written (`setosa`, spaces around it allowed), or its number; one of `bool` type, 0 or 1 |
| a `char` array of n | the cell's text, unescaped, of at most n − 1 bytes, the rest zeroed |
| an `int` with `[[csv::category]]` | its column's category code ([categories](#categories)) |

**A cell not read** (empty, short, no value of the field's kind, or outside its `fminus::range`) sets the field to its
`fminus::default`; a `float` or a `double` without one gets NaN, which shows in every sum it reaches, where `json_read`
would leave it as it was; any other field without one is left as it was. A field's range and default apply to each of
its columns.

**The result** is how many rows had every field without a default filled from its cell: `json_read`'s bool counted over
rows, so `if (csv_read(doc, rows, n) != n)` is the check. A field without a default whose column the header does not
have is `CSV_MISSING`, and nothing is written; a field with one gets its default in every row. A null document reads
no rows (0). `CSV_INVALID` is a table the runtime refuses, which the compiler never makes, or no memory.

**Left out**, as `json_read` leaves them: a `param`, a field with `[[csv::skip]]`, a `string` (it would point into the
document, which the rows outlive), a pointer, a handle and a poly type. A module with no field `csv_read` reads, or with
more than 1024 columns, is an error at the call.

**Rows convert at once.** Finding where rows begin is one pass in order, since a quote changes what a line end means;
converting them is independent row by row, so `csv_read` converts blocks of 1024 rows on FMinus's pool
(`src/runtime/Parallel.cpp`), each row written by one thread: the same rows on any number of threads.

## Categories

A column of text whose values are the data's, not the program's (cities, product codes), is read as categories: each
distinct text a code from 0, in the order the rows first have it (pandas' `factorize`), an empty cell none.

```c
module Trip { float fare; int city [[csv::category]] [[fminus::default(-1)]]; };
...
int count[64];
for (int i = 0; i < n; ++i)
    if (trips[i].city >= 0 && trips[i].city < 64)
        count[trips[i].city] += 1;                         // a group-by is an array indexed by the code
for (int k = 0; k < csv_levels(doc, "city") && k < 64; ++k)
    printf("%-12s %d\n", csv_label(doc, "city", k), count[k]);
```

`[[csv::category]]` applies to an `int` field or an array of ints (an enumeration reads its constants' names without
it). The document keeps each column's categories, made the first time a read asks for them (`csv_read`, `csv_levels`,
`csv_label`), in row order, before any rows convert: the same text has the same code on any number of threads. Two
documents' codes for one text are not the same number.

## The schema, written for you

`fminus data.csv --emit fm` writes the module a table's header and rows suggest (`.tsv` splits at tabs), for the
program to keep, so the file's shape is checked against the program when it compiles, as `fminus pedal.cir --emit fm`
writes a circuit's module:

```c
// iris.csv: 150 rows, 5 columns. Written by fminus --emit fm: check it, and keep it in the program.
enum species_value { setosa, versicolor, virginica };

module IrisRow
{
    float sepal_length;                 // 4.3 to 7.9
    float sepal_width;                  // 2 to 4.4
    float petal_length;                 // 1 to 6.9
    float petal_width;                  // 0.1 to 2.5
    enum species_value species;         // 3 values
};
```

Each column is read through the runtime's own reader, its distinct texts as `csv_read` sees them, and typed by them:
integers an `int`; other numbers a `float`, or a `double` when a value writes more than 7 significant digits or is past
a float's range; `true` and `false` a `bool`; at most 16 distinct words that can be an enumeration's constants (and
are no keyword, nor another column's constant) an enumeration; any other text an `int` with `[[csv::category]]`. A name
that is no identifier, or is a keyword, becomes one (`Pickup Hour` is `Pickup_Hour`, `1st` is `_1st`, `float` is
`float_`) with `[[csv::name("...")]]`; a second column of a name the header has already gets `[[csv::skip]]` and a
comment saying to read it by its index. The comments are what was seen: a range (each end the shortest digits that read
back as it), empty cells, how many values or texts. The ranges are comments, not `fminus::range`: a range would turn
tomorrow's larger value into a missing one. The whole file is read, since a sample could miss the one row that makes a
column a `double`.

## NumPy's .npy

`npy_floats(data, bytes, out, cap)` reads what `numpy.save` writes: versions 1.0 to 3.0, of little-endian `float32`,
`float64` or `int32` values in C order, into floats, the first `cap`; it gives how many values the file holds (its
shape's product), or -1 for bytes that are no such file (another type, big-endian, Fortran order, a header or values
cut short). `npy_shape(data, bytes, axis)` gives dimension `axis` (from 0), or with -1 how many dimensions there are
(0 for a scalar), or -1. Both read only the bytes they are given, wait-free:

```c
file_content f = file_read("weights/w.npy");
defer file_close(f);
const int rows = npy_shape(file_data(f), file_size(f), 0), cols = npy_shape(file_data(f), file_size(f), 1);
npy_floats(file_data(f), file_size(f), w, 64 * 64);       // rows × cols floats, row by row
```

## The audio thread

Parsing, freeing, `csv_read` and the categories allocate, and are refused in a `[[clang::nonallocating]]` or
`[[clang::nonblocking]]` function. Reading a column (`csv_floats`, `csv_ints`, ...), a name, a count, and an `.npy`
file's values are wait-free, allowed in both: their time is bounded by the document, not by a constant (a column's read
walks each row to its cell). Load a table off the audio thread, into arrays, and keep the audio thread to those.

## What it costs

Measured once, on the 4-core x86-64 Linux container this was written in: a file of 10⁶ rows of five columns (an int, two
numbers, a category of eight cities, an int), 26 MB, read by an executable: the whole process, reading the file and
parsing it, about 0.05 to 0.07 s; with `csv_read` into an array of the module too, about 0.12 to 0.15 s. The same file
took pandas 3.0.5's `read_csv` (with the city a category) about 0.19 s, and Python's `csv.reader` about 0.84 s. One
machine, a few runs each: an order of magnitude, not a benchmark.

## How it is built

| File | What it is |
|---|---|
| `src/runtime/Csv.cpp` | the reader: the document and its row index (`fminus_rt_csv_parse`), a row's cells, the column reads, the categories (`levelsOf`), `fminus_rt_csv_read` (a table's columns checked by `JsonShapes.h`, blocks of rows on the pool) |
| `src/runtime/Npy.cpp` | `.npy`'s header and values |
| `src/runtime/JsonShapes.h` | the table's records, `json_read`'s, and `FieldCategory`, a flag only `csv_read`'s tables carry |
| `src/ir/Csv.cpp` | the statuses, the `csv::` attributes, a column and a delimiter as arguments, `csv_read`'s table of a module's columns |
| `src/ir/Calls.cpp` | the builtins' entries, and what LLVM is told each runtime function touches (`CsvParse` keeps its text's address; `CsvRead` and `CsvFill` read memory no argument points to, the text) |
| `src/api/CsvSchema.cpp` | `--emit fm`'s module of a table, for `tools/fminus/main.cpp` |

[extending.md](extending.md), recipe 53, walks through the code; the tests are `tests/unit/CsvTest.cpp` (the reader),
`CsvReadTest.cpp` (through the JIT), `NpyTest.cpp`, and `tests/lit/fminus/csv.fm` and `csv-schema.fm`.

## What is not built

| Left out | Why | What would bring it in |
|---|---|---|
| Writing tables (a CSV literal, `.npy` out) | step 4 of the plan | `file_write("out/x.csv", csv (rows, n))`, as the JSON literal is written |
| A table larger than memory | whole files up to the file policy's limit (256 MiB by default, 2 GiB at most) cover a personal project's data | the file interface's partial reads, and a reader that keeps a row's tail between parts |
| Dates and times | a date column is text (a category) or a number today | a `[[csv::date]]` field, days since 1970, ISO 8601 only |
| Joins on text across files | two documents' category codes differ | a recode of one document's codes into another's |
| A file without a header, comments, another quote character | RFC 4180 has none of them | an option each to `csv_parse` |
| `.npy` of other types, Fortran order, `.npz` | float32, float64 and int32 in C order cover what a model's weights and a dataset are | a type each; `.npz` is a zip archive, a dependency |
