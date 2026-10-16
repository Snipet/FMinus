# Recipes as patches

The recipes of [../extending.md](../extending.md), as made and verified: built with `-Werror`, and tested in the dev
and asan presets with all 19 tests passing. They apply in order to the commit that added them (each assumes the ones
before it):

| Patch | Recipes |
|---|---|
| `1-builtin-and-operator.patch` | 1, a builtin (`tanh`); 2, an operator (`%`) |
| `2-comparisons-bool-if.patch` | 3, comparisons, `bool` and an `if` expression |
| `3-var-assignment-while.patch` | 4, `var`, assignment and `while` |
| `4-runtime-function-and-pass.patch` | 5, a runtime function (`db_to_gain`); 6, an LLVM pass (`CountFlopsPass`) |

```bash
git switch -c try-recipes
git apply docs/recipes/1-builtin-and-operator.patch   # and so on, in order
cmake --workflow --preset dev
```

Their tests go in `tests/unit/RecipeTest.cpp`. Once the compiler moves on, they stop applying, but they remain
readable diffs of one way to make each change.
