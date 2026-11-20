# Stage 2: the preprocessor rewrites text

`cc_preprocess_file` takes a path and returns a `char *`. Between those two
points it reads the file a line at a time, expands macros with string
substitution into a growing buffer, splices included files into the same
buffer, and drops directive lines in favor of a newline so line counts stay
roughly aligned. `main.c` then hands that buffer to `cc_lex_source` and the
pipeline continues from there.

## The decision

The preprocessor works on lines and strings, not on tokens. A macro stores its
replacement as `char *replacement`. `#define WIDE(x) ((x) * 2)` against
`WIDE(A)` is a string walk that finds the identifier, matches the parentheses,
copies the argument text in, and recursively expands the result up to
`CC_MAX_MACRO_EXPANSION_DEPTH` (32). Pasting with `##` is a string operation,
`cc_collapse_token_paste`, that deletes whitespace around the join. `#include`
recurses into `cc_preprocess_path` and appends the header's lines inline.

The alternative was to expand at the token level: keep the lexer's token list,
substitute tokens in place, and carry a source map so each expanded token still
knew the file, line and column it came from. That is more machinery to build
and to keep correct, and the shape I chose fits in one file and reads like the
C standard's own description of the stage.

The price is location fidelity, and you can see it without reading the code:

```text
loc.c:6:9: error: cannot assign value of type int to pointer
  p = ((1) * 2);
        ^~~~~~
```

The user wrote `p = WIDE(A);` on that line. The path in the message is right,
because each file's own directives are reported from the line loop, which tracks
the real path and line number. Everything after preprocessing is reported
against the expanded buffer, so the quoted line and the carets describe text
that is not in the file. The smoke tests assert on messages and not on caret
columns, so the suite stayed green while this was true.

## The bug

Includes recurse, and nothing capped that recursion. `#pragma once` was the only
cycle breaker, and it only works when the header opts in. Two headers including
each other, or one header including itself, walked deeper until the process ran
out of stack. Commit `a06bc23` (the one with the `+++++++++` message) added a
counter to `CCPreprocessor` and a guard at the top of `cc_preprocess_path`:

```c
if (preprocessor->include_depth >= CC_MAX_INCLUDE_DEPTH) {
    cc_add_diagnostic(preprocessor, path, 0, 1, 1, 1, "include nesting too deep");
    return;
}
```

with an increment on the way in and a decrement on the way out, capped at 200.
`tests/include_cycle_a.c` and `tests/include_cycle_b.c`, added in the same
commit, are a pair of files that include each other. Running the built binary on the first one now reports
`tests/include_cycle_a.c:1:1: error: include nesting too deep` instead of dying.

Worth keeping in mind when you add to this file: a text-rewriting preprocessor
cannot tell that the file it is about to read is already open unless something
explicitly remembers that, and `#pragma once` only helps when the header
cooperates.

## What the next stage had to work around

The parser receives a flat token stream scanned from the expanded buffer, with
no record of which file or which macro produced any token, so every syntax error
in a macro-heavy file is anchored to text the user did not write.
