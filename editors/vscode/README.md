# FMinus for VSCode

Syntax highlighting for FMinus (`.fm`): keywords, types (`float`, `int`, `bool`, `void`, and the SIMD recipes'
`float4` and `poly float`) and qualifiers (`const`, `restrict`, `poly`), modules (`module OnePole`, with the name it
declares, and `this`), `true` and `false`, C's constants including hex floats and hex integers (and `2f` marked as the
error it is), operators (`++`, `&`, `[]`, `->`, `sizeof` too), attributes (`[[clang::nonblocking]]`), builtins
(`sinf`, `tanhf`, `malloc`, `reduce_add`, ...), function and method names, and swizzles and fields (`v.wzyx`,
`p->y`). In lit tests, `RUN:`, `REQUIRES:`, `CHECK:` and the other check prefixes stand out inside their comments.
`//` and `/* */` toggle as comments, and brackets match and close.

It is declarative: three JSON files, no code, nothing that runs on your files.

| File | |
|---|---|
| `package.json` | the extension: a language `fminus` for `.fm` files, and its grammar |
| `language-configuration.json` | comments, brackets, auto-closing, indentation |
| `syntaxes/fminus.tmLanguage.json` | the TextMate grammar: one rule per kind of token, in the order they are tried |
| `package.py` | packages the folder as a `.vsix`, without npm or the network |

## Installing

```bash
python3 editors/vscode/package.py                               # build-vscode/fminus-0.5.0.vsix
code --install-extension build-vscode/fminus-0.5.0.vsix
```

Then reload the window (Developer: Reload Window). `code` comes with VSCode; if it isn't on PATH, run "Shell Command:
Install 'code' command in PATH" from the command palette, or use the Extensions view's "..." menu, "Install from
VSIX...". To update, bump `version` in `package.json`, package again and install again.

## When the language grows

A new keyword or type is a word in the `keywords` or `types` rule of the grammar; a builtin is a name in `builtins`; an
operator goes in `operators`, longer spellings before their prefixes (`<=` before `<`). Rules are tried in the order of
the top-level `patterns`, so keywords come before `functions` (otherwise `if (` would look like a call).

To see the scopes a token gets, run "Developer: Inspect Editor Tokens and Scopes" from the command palette.
