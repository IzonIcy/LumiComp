# Stage 1: tokens are offsets, not text

A token, from `include/ccompiler/token.h`, is two fields:

```c
typedef struct {
    CCTokenKind kind;
    CCSpan span;
} CCToken;
```

`CCSpan` is an offset, a line, a column and a length. There is no pointer to the
bytes, no owned copy of the spelling, and no parsed value.

## The decision

Tokens are offsets into the source buffer, and the lexer never evaluates a
literal. The alternative was to make each token own its text, or to intern
identifiers once and hand out an id, or to turn `0x1f` into a number during the
scan. Offsets won because they keep the scanner close to allocation-free: the
token buffer is the only thing that grows, the file stays the single record of
what was written, and diagnostics can underline any range of the original bytes
without a second copy to keep in sync.

The cost is paid by everything downstream. Any stage that wants the spelling has
to copy it back out of the buffer, so `cc_token_text` exists and `parser.c` calls
it nine times (counted with `grep -c cc_token_text src/ccompiler/parser.c`).
Nothing that consumes a token can be trusted without the buffer it came from,
which is one reason the preprocessor builds a fresh buffer instead of handing
the parser a spliced token list (that is the next post). The value of a number
literal is not known at scan time either, so it gets parsed again much later,
from text, by another file.

Recognition is plain: a keyword table of 44 entries and a punctuator table of
55 entries ordered longest first, both scanned linearly with `memcmp`. The
longest-first order is what makes `>>=` win over `>>` without any backtracking,
and the digraphs (`<:`, `%:`, `%:%:`) fit in the same table.

## The bug

There is no bug in the scanning logic in this history. The closest thing, and
the only defect that ever stopped the lexer from running at all, is a rename.
Commit `c9bebda` ("Made Some Wording Fixes!") rewrote every include in the tree
from `ccompiler/...` to `C-Compiler/...`, and changed the Makefile to glob
`src/C-Compiler/*.c`. Neither path exists. The lexer's first line pointed at a
header that could not be found, `make` produced no objects, and nothing else in
the pipeline could run either. Commit `8791edd`, four minutes later, put the
paths back and renamed the header guards that had picked up the hyphen, since
`C-Compiler_LEXER_H` is not a macro name that survives a strict preprocessor.

I count this one because it is real, it is in the diff, and it is the failure
mode this stage shares with the rest of the tree: a stage is available or it is
not, and the thing that decides is one include line. It also shows what the test
suite cannot catch, because every test in the repo starts by building the
binary. What it does not do is teach anything about lexing, which is why this
post says plainly that the lexer has no logic bug in its history worth citing.

## What the next stage had to work around

Since a token only means something against the buffer it was scanned from, the
preprocessor could not assemble one token list from several files, so it did the
opposite: it rewrote the program as text and asked the lexer to scan the result
again.
