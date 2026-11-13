# FMinus for VSCode

Syntax highlighting for FMinus (`.fm`): keywords (`switch`, `case`, `default`, `parallel`, `defer`, `serve`, `route`, `reply`, `json` and `when` among them), types
(`float`, `double`, `int`, `bool`, `void`, the SIMD recipes' `float4` and `poly float`, the handles, `http_response`,
`json_doc`, `fft_plan` and the rest, `string` and `char`) and qualifiers (`const`,
`restrict`, `poly`, `param`), modules (`module OnePole`, with the name it declares, and `this`), enumerations
(`enum Mode`, with its tag), `true` and `false`, C's constants including hex floats and hex integers (and `2f` marked as
the error it is), operators (`++`, `&`, `[]`, `->`, `sizeof` too, and the bitwise `|`, `^`, `~`, `<<`, `>>` and their
assignments), attributes (`[[clang::nonblocking]]`, a parameter's `[[fminus::range(20, 20000)]]` with its numbers, and
a field's in any namespace, `[[ui::unit("dB")]]`, with its strings and their escapes, an unknown escape marked),
string literals in code too, a builtin's arguments, with a format's places (`%d`, `%%`, a reply's `%g` and `%.9g`, and a route's holes, `{name}`),
builtins (`sinf`, `tanhf`, the double `sqrt` and `fma`, `malloc`, `fft_forward`, `hash`, `randf`, `bytes_floats`,
`json_float`, `xml_float`, `http_get`, the server's `http_listen`, `http_next`, `http_is` and `http_reply_text`, the
file interface's `file_read`, `file_list` and `file_write`, a string's `string_size`, `string_slice` and `string_of`,
`reduce_add`, ...) and the web library's and the file
interface's status codes (`HTTP_REFUSED`, `FILE_REFUSED`, ...),
function and method names, and swizzles and fields (`v.wzyx`, `p->y`), and the directives: `#include "file"`, `#pragma`,
the conditionals (`#if`, `#ifdef`, `#ifndef`, `#elif`, `#elifdef`, `#elifndef`, `#else`, `#endif`, with `defined` and
the macros' names in their conditions) and `#define` and `#undef` (any other is marked as the error it is). In lit
tests, `RUN:`, `REQUIRES:`, `CHECK:` and the other check prefixes stand out inside their comments. `//` and `/* */`
toggle as comments, brackets match and close, and so do a string's quotes.

It is declarative: three JSON files, no code, nothing that runs on your files.

| File | |
|---|---|
| `package.json` | the extension: a language `fminus` for `.fm` files, and its grammar |
| `language-configuration.json` | comments, brackets, auto-closing, indentation |
| `syntaxes/fminus.tmLanguage.json` | the TextMate grammar: one rule per kind of token, in the order they are tried |
| `package.py` | packages the folder as a `.vsix`, without npm or the network |

## Installing

```bash
python3 editors/vscode/package.py                               # build-vscode/fminus-0.22.0.vsix
code --install-extension build-vscode/fminus-0.22.0.vsix
```

Then reload the window (Developer: Reload Window). `code` comes with VSCode; if it isn't on PATH, run "Shell Command:
Install 'code' command in PATH" from the command palette, or use the Extensions view's "..." menu, "Install from
VSIX...". To update, bump `version` in `package.json`, package again and install again.

## When the language grows

A new keyword or type is a word in the `keywords` or `types` rule of the grammar; a builtin is a name in `builtins`; an
operator goes in `operators`, longer spellings before their prefixes (`<<=` before `<<` before `<=` before `<`). Rules
are tried in the order of the top-level `patterns`, so keywords come before `functions` (otherwise `if (` would look
like a call).

To see the scopes a token gets, run "Developer: Inspect Editor Tokens and Scopes" from the command palette.
