# FMinus's documentation site

The site at <https://snipet.github.io/FMinus/>: the public API, the grammar and the internals, generated from this
repository every time it builds, so it can't drift from the code. It is a SvelteKit 2 app (Svelte 5), prerendered to
static files by `@sveltejs/adapter-static`, and `.github/workflows/pages.yml` deploys it on every push to `main` (the
docs link into most of the repository, and the build checks every link, so any push can change the site).

## Running it

Node 24 or later, from this directory:

```bash
npm ci              # the exact packages of package-lock.json
npm run dev         # generate, then a dev server at http://localhost:5173/ (edits to src/ reload; rerun for sources)
npm run build       # generate, then the static site in build/
npm run preview     # serve build/ at http://localhost:4173/
npm run check       # after a build: every link and anchor resolves, every declaration, rule and token is on its page
```

The base path comes from `BASE_PATH`: nothing locally, `/FMinus` on GitHub Pages. To see the site as Pages serves it:

```bash
BASE_PATH=/FMinus npm run build && BASE_PATH=/FMinus npm run check && BASE_PATH=/FMinus npm run preview
# then http://localhost:4173/FMinus/
```

`vite preview` reads `build/` when it starts: restart it after a build.

## How the content is generated

`scripts/generate.js` runs before `vite dev` and `vite build`. It reads the repository at `..` (this directory's
parent, never a machine path) and writes JSON into `src/lib/generated/`, which is ignored by git and rebuilt from
nothing each time. The pages import that JSON. Everything is rendered and highlighted at build time; the browser runs
only the search box, the rule filter and SvelteKit's navigation.

| Generator | Reads | Writes |
|---|---|---|
| `scripts/lib/api.js` (with `cxx-header.js`) | `include/fminus/FMinus.h` | `api.json`: the header's opening comment (the introduction), and every public declaration in `namespace fminus`, nested as in the header, with its signature and its comment |
| `scripts/lib/grammar.js` (with `bison.js`, `flex.js`, `railroad.js`) | `src/frontend/Parser.y`, `src/frontend/Lexer.l` | `grammar.json`: Parser.y's summary, every rule without its C++ actions, a railroad diagram of each (SVG), every token with its spelling and the patterns of Lexer.l that return it, the precedence, and the scanner's rules |
| `scripts/lib/docs.js` | `docs/*.md`, `docs/recipes/`, the README's language section and opening paragraph | `internals.json`, `docs/<page>.json`, `home.json` |
| `scripts/lib/highlight.js` | `editors/vscode/syntaxes/fminus.tmLanguage.json` | `highlight.css`: Shiki's colours as classes, light and dark |
| `scripts/generate.js` | the above | `search.json`: every heading and declaration, for the search box |

- **The API.** The header's comments are its documentation, kept word for word; only the source's line breaks go,
  so that prose reflows. A comment's indented blocks keep their shape: C++ is highlighted, a list of terms
  (`Knob  the slot's value, ...`) becomes a definition list, and aligned columns become a table. `cxx-header.js`
  reads the header as its style writes it (a comment above a declaration, or after it on its line) and stops the build
  with the file and line of anything it can't place, such as two names in one declaration (`int a, b;`). Every public
  declaration needs a comment. One without its own is documented by the overload just above it; by the field above it
  when the two share a comment (a sentence that runs on from one's comment into the next's, as `minimum` and `maximum`
  do, or parallel fields of one type, `streamOffset` and `valueOffset`); by its type's or namespace's comment when
  that comment names it (in a `code` span, as a term of an indented list, or as a name that can't be a word, such as
  `valueOffset`); by its members when every one of them has one (`Options`); or, for `Program`, by the header's
  opening comment. A destructor and a deleted copy need none. A macro defined in an `#if` block is documented by the
  comment above the block when that names it. The members the header leaves to their type's comment without naming
  them today are listed in `api.js` (`COVERED`); any other declaration without a comment stops the build, naming it.
- **The grammar.** `bison.js` reads the declarations and the rules (each alternative's symbols, `%empty`, `%prec`, a
  mid-rule action as a marker) and `flex.js` the definitions and the rules in order, with what each action does: the
  token it returns (`Parser::make_X`), the error it throws, the start condition it enters, or nothing. A symbol that
  is neither a rule nor a `%token`, or a token the scanner returns that Parser.y doesn't declare, stops the build. A
  comment above a rule, or after its `;`, is the rule's; inside a rule, one on its own line is the alternative's after
  it, one at the end of a line the alternative on it, and one on its own line in an action is shown as the action's.
  A comment that documents nothing (a blank line under it, or after a rule's last alternative) stops the build. The
  railroad diagrams (`railroad.js`, after Tab Atkins's railroad-diagrams) draw a left-recursive list as a loop.
- **The internals.** Markdown is rendered by markdown-it with GitHub's heading ids, so `building.md#windows` still
  lands. Links between documents go to the site's pages, and links and `paths` to the repository's files go to
  GitHub (`https://github.com/Snipet/FMinus/blob/main/<path>`). A link to a heading or file that isn't there stops
  the build. Code fences are highlighted with Shiki: `c` fences, which are FMinus programs, with the VSCode extension's
  grammar; `cpp`, `llvm`, `cmake`, `bash`, `asm` and the recipes' `diff` with Shiki's; `yacc` and `lex` with two
  small grammars in `highlight.js`.
- **Adding a document.** A new `docs/x.md` becomes `/internals/x/` on its own; listing it in `docs/README.md`'s table
  gives it its place and its description.

The prerendering also fails on a link to a page or an anchor that doesn't exist (`svelte.config.js`).

## Layout

```
scripts/             the generators (Node, no dependencies beyond package.json's) and check.js
src/app.html         the page shell; src/app.css the styles (light and dark, as the system is)
src/lib/components/  ApiEntry, Search, Sidebar, DocsNav
src/lib/generated/   the generators' output (ignored by git)
src/routes/          / (home), /api/, /grammar/, /internals/ and /internals/[slug]/
static/              the favicon, and .nojekyll
```
