# Recipes as patches

The recipes of [../extending.md](../extending.md), as made and verified: built with `-Werror`, and tested in the dev
and asan presets with all 22 tests passing. They apply in order to the commit that added them (each assumes the ones
before it):

| Patch | Recipes |
|---|---|
| `1-builtin-and-operator.patch` | 1, a builtin (`tanhf`); 2, an operator (`%` and `%=`) |
| `2-bool-logic-conditional.patch` | 3, `bool` (`true`, `false`), the comparisons, `!`, `&&`, `\|\|` and `?:` |
| `3-if-while-for.patch` | 4, `if`/`else`, `while`, `for`, `break` and `continue` |
| `4-runtime-function-and-pass.patch` | 5, a runtime function (`db_to_gain`, by prototype or builtin); 6, an LLVM pass (`CountFlopsPass`) |

```bash
git switch -c try-recipes
git apply docs/recipes/1-builtin-and-operator.patch   # and so on, in order
cmake --workflow --preset dev
```

Their tests go in `tests/unit/RecipeTest.cpp`. Once the compiler moves on, they stop applying, but they remain
readable diffs of one way to make each change.
